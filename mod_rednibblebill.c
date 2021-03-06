/* 
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2012, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 *
 * The Initial Developer of this module is
 * Ross McKillop <ross@rsmck.co.uk>
 * Based on the works of Darren Schreiber <d@d-man.org>
 *
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 * Darren Schreiber <d@d-man.org>
 * Rupa Schomaker <rupa@rupa.com>
 * Ross McKillop <ross@rsmck.co.uk>
 *
 * mod_rednibblebill.c - Nibble Billing using REDIS
 * Purpose is to allow real-time debiting of credit or cash from a redis key while calls are in progress. 
 *
 * This is essentially a redis-backed fork of the mod_nibblebill by Darren Schreiber <d@d-man.org>
 * it provides the same functionality as mod_nibblebill but using a REDIS back end.
 *
 */

#include <switch.h>
#include "credis.h"

typedef struct {
	switch_time_t lastts;		/* Last time we did any billing */
	double total;				/* Total amount billed so far */

	switch_time_t pausets;		/* Timestamp of when a pause action started. 0 if not paused */
	double bill_adjustments;	/* Adjustments to make to the next billing, based on pause/resume events */

	int lowbal_action_executed;	/* Set to 1 once lowbal_action has been executed */
} rednibble_data_t;


typedef struct rednibblebill_results {
	double balance;

	double percall_max;			/* Overrides global on a per-user level */
	double lowbal_amt;			/*  ditto */
} rednibblebill_results_t;


/* Keep track of our config, event hooks and database connection variables, for this module only */
static struct {
	/* Memory */
	switch_memory_pool_t *pool;

	/* Event hooks */
	switch_event_node_t *node;

	/* Global mutex (don't touch a session when it's already being touched) */
	switch_mutex_t *mutex;

	/* Global billing config options */
	double percall_max_amt;		/* Per-call billing limit (safety check, for fraud) */
	char *percall_action;		/* Exceeded length of per-call action */
	double lowbal_amt;			/* When we warn them they are near depletion */
	char *lowbal_action;		/* Low balance action */
	double nobal_amt;			/* Minimum amount that must remain in the account */
	char *nobal_action;			/* Drop action */

	/* Other options */
	int global_heartbeat;		/* Supervise and bill every X seconds, 0 means off */

	/* Database settings */
	char *redis_host;
	int redis_port;
	int redis_timeout;
} globals;

static void rednibblebill_pause(switch_core_session_t *session);

/**************************
* Setup FreeSWITCH Macros *
**************************/
/* Define the module's load function */
SWITCH_MODULE_LOAD_FUNCTION(mod_rednibblebill_load);

/* Define the module's shutdown function */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_rednibblebill_shutdown);

/* Define the module's name, load function, shutdown function and runtime function */
SWITCH_MODULE_DEFINITION(mod_rednibblebill, mod_rednibblebill_load, mod_rednibblebill_shutdown, NULL);

/* String setting functions */
SWITCH_DECLARE_GLOBAL_STRING_FUNC(set_global_redis_host, globals.redis_host);
SWITCH_DECLARE_GLOBAL_STRING_FUNC(set_global_percall_action, globals.percall_action);
SWITCH_DECLARE_GLOBAL_STRING_FUNC(set_global_lowbal_action, globals.lowbal_action);
SWITCH_DECLARE_GLOBAL_STRING_FUNC(set_global_nobal_action, globals.nobal_action);

static switch_status_t load_config(void)
{
	char *cf = "rednibblebill.conf";
	switch_xml_t cfg, xml = NULL, param, settings;
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	if (!(xml = switch_xml_open_cfg(cf, &cfg, NULL))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "open of %s failed\n", cf);
		status = SWITCH_STATUS_SUCCESS;	/* We don't fail because we can still write to a text file or buffer */
	}

	if ((settings = switch_xml_child(cfg, "settings"))) {
		for (param = switch_xml_child(settings, "param"); param; param = param->next) {
			char *var = (char *) switch_xml_attr_soft(param, "name");
			char *val = (char *) switch_xml_attr_soft(param, "value");

			if (!strcasecmp(var, "redis_host")) {
				set_global_redis_host(val);
			} else if (!strcasecmp(var, "redis_port")) {
				globals.redis_port = atoi(val);
			} else if (!strcasecmp(var, "redis_timeout")) {
				globals.redis_timeout = atoi(val);
			} else if (!strcasecmp(var, "percall_action")) {
				set_global_percall_action(val);
			} else if (!strcasecmp(var, "percall_max_amt")) {
				globals.percall_max_amt = atof(val);
			} else if (!strcasecmp(var, "lowbal_action")) {
				set_global_lowbal_action(val);
			} else if (!strcasecmp(var, "lowbal_amt")) {
				globals.lowbal_amt = atof(val);
			} else if (!strcasecmp(var, "nobal_action")) {
				set_global_nobal_action(val);
			} else if (!strcasecmp(var, "nobal_amt")) {
				globals.nobal_amt = atof(val);
			} else if (!strcasecmp(var, "global_heartbeat")) {
				globals.global_heartbeat = atoi(val);
			}
		}
	}

	if (zstr(globals.percall_action)) {
		set_global_percall_action("hangup");
	}
	if (zstr(globals.lowbal_action)) {
		set_global_lowbal_action("play ding");
	}
	if (zstr(globals.nobal_action)) {
		set_global_nobal_action("hangup");
	}

	if (xml) {
		switch_xml_free(xml);
	}
	return status;
}

static switch_status_t redis_factory(REDIS *redis) 
{
	if (!((*redis) = credis_connect(globals.redis_host, globals.redis_port, globals.redis_timeout))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't connect to redis server at %s:%d timeout:%d\n", globals.redis_host, globals.redis_port, globals.redis_timeout);
		return SWITCH_STATUS_FALSE;
	} 
        
	return SWITCH_STATUS_SUCCESS;
}

void debug_event_handler(switch_event_t *event)
{
	if (!event) {
		return;
	}

	/* Print out all event headers, for fun */
	if (event->headers) {
		switch_event_header_t *event_header = NULL;
		for (event_header = event->headers; event_header; event_header = event_header->next) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Header info: %s => %s\n", event_header->name, event_header->value);
		}
	}
}

static switch_status_t exec_app(switch_core_session_t *session, const char *app_string)
{
	switch_status_t status;
	char *strings[2] = { 0 };
	char *dup;

	if (!app_string) {
		return SWITCH_STATUS_FALSE;
	}

	dup = strdup(app_string);
	switch_assert(dup);
	switch_separate_string(dup, ' ', strings, sizeof(strings) / sizeof(strings[0]));
	status = switch_core_session_execute_application(session, strings[0], strings[1]);
	free(dup);
	return status;
}

static void transfer_call(switch_core_session_t *session, char *destination)
{
	char *argv[4] = { 0 };
	const char *uuid;
	switch_channel_t *channel = switch_core_session_get_channel(session);
	char *mydup;

	if (!destination) {
		return;
	}

	mydup = strdup(destination);
	switch_assert(mydup);
	switch_separate_string(mydup, ' ', argv, (sizeof(argv) / sizeof(argv[0])));

	/* Find the uuid of our B leg. If it exists, transfer it first */
	if ((uuid = switch_channel_get_partner_uuid(channel))) {
		switch_core_session_t *b_session;

		/* Get info on the B leg */
		if ((b_session = switch_core_session_locate(uuid))) {
			/* Make sure we are in the media path on B leg */
			switch_ivr_media(uuid, SMF_REBRIDGE);

			/* Transfer the B leg */
			switch_ivr_session_transfer(b_session, argv[0], argv[1], argv[2]);
			switch_core_session_rwunlock(b_session);
		}
	}

	/* Make sure we are in the media path on A leg */
	uuid = switch_core_session_get_uuid(session);
	switch_ivr_media(uuid, SMF_REBRIDGE);

	/* Transfer the A leg */
	switch_ivr_session_transfer(session, argv[0], argv[1], argv[2]);
	free(mydup);
}

/* At this time, billing never succeeds if you don't have a database. */
static switch_status_t bill_event(double billamount, const char *billaccount, switch_channel_t *channel)
{
	REDIS redis;
	char *rediskey;
	int val;
        int dec;
        switch_status_t status = SWITCH_STATUS_FALSE;

	if (redis_factory(&redis) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	rediskey = switch_mprintf("rn_%s", billaccount);
	dec = (int)ceil(billamount*1000000);
	

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Updating account %s by %e\n", billaccount, billamount);
	
	if (credis_decrby(redis, rediskey, dec, &val) != 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "ERR: Could not decrement redis value on key %s by %e\n", rediskey, billamount);
		status = SWITCH_STATUS_FALSE;
	} else {
		status = SWITCH_STATUS_SUCCESS;
	}

	credis_close(redis);
	return status;
}


static double get_balance(const char *billaccount, switch_channel_t *channel)
{
	REDIS redis;
	char *rediskey;
	char *str;
	double val;
	int result;

	double balance = 0.0;

	if (redis_factory(&redis) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

       	rediskey = switch_mprintf("rn_%s", billaccount);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Looking up redis key %s\n", rediskey);

	result = credis_get(redis, rediskey, &str);

	if (result != 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "ERR: Could not get redis value on key %s (got result %d) - returning positive value for now (FIXME)\n", rediskey, result);
		balance = 1.0;
	} else {
		val = atof(str);
		balance = val/1000000;
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Key %s returned %s converted to %e / %f \n", rediskey, str, val, balance);
	}

	switch_safe_free(rediskey);
	credis_close(redis);

	return balance;
}

/* This is where we actually charge the guy 
  This can be called anytime a call is in progress or at the end of a call before the session is destroyed */
static switch_status_t do_billing(switch_core_session_t *session)
{
	/* FS vars we will use */
	switch_channel_t *channel;
	switch_caller_profile_t *profile;

	/* Local vars */
	rednibble_data_t *rednibble_data;
	switch_time_t ts = switch_micro_time_now();
	double billamount;
	char date[80] = "";
	char *uuid;
	switch_size_t retsize;
	switch_time_exp_t tm;
	const char *billrate;
	const char *billincrement;
	const char *billaccount;
	double nobal_amt = globals.nobal_amt;
	double lowbal_amt = globals.lowbal_amt;
	double balance;

	if (!session) {
		/* Why are we here? */
		return SWITCH_STATUS_SUCCESS;
	}

	uuid = switch_core_session_get_uuid(session);

	/* Get channel var */
	if (!(channel = switch_core_session_get_channel(session))) {
		return SWITCH_STATUS_SUCCESS;
	}

	/* Variables kept in FS but relevant only to this module */
	billrate = switch_channel_get_variable(channel, "rednibble_rate");
	billincrement = switch_channel_get_variable(channel, "rednibble_increment");
	billaccount = switch_channel_get_variable(channel, "rednibble_account");
	
	if (!zstr(switch_channel_get_variable(channel, "nobal_amt"))) {
		nobal_amt = atof(switch_channel_get_variable(channel, "nobal_amt"));
	}
	
	if (!zstr(switch_channel_get_variable(channel, "lowbal_amt"))) {
		lowbal_amt = atof(switch_channel_get_variable(channel, "lowbal_amt"));
	}
	
	/* Return if there's no billing information on this session */
	if (!billrate || !billaccount) {
		return SWITCH_STATUS_SUCCESS;
	}

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Attempting to bill at %s per minute to account %s\n", billrate,
					  billaccount);

	/* Get caller profile info from channel */
	profile = switch_channel_get_caller_profile(channel);

	if (!profile || !profile->times) {
		/* No caller profile (why would this happen?) */
		return SWITCH_STATUS_SUCCESS;
	}

	if (profile->times->answered < 1) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Not billing %s - call is not in answered state\n", billaccount);

		/* See if this person has enough money left to continue the call */
		balance = get_balance(billaccount, channel);
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Comparing %f to hangup balance of %f\n", balance, nobal_amt);
		if (balance <= nobal_amt) {
			/* Not enough money - reroute call to nobal location */
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Balance of %f fell below allowed amount of %f! (Account %s)\n",
							  balance, nobal_amt, billaccount);

			transfer_call(session, globals.nobal_action);
		}

		return SWITCH_STATUS_SUCCESS;
	}

	/* Lock this session's data for this module while we tinker with it */
	if (globals.mutex) {
		switch_mutex_lock(globals.mutex);
	}

	/* Get our rednibble data var. This will be NULL if it's our first call here for this session */
	rednibble_data = (rednibble_data_t *) switch_channel_get_private(channel, "_rednibble_data_");

	/* Are we in paused mode? If so, we don't do anything here - go back! */
	if (rednibble_data && (rednibble_data->pausets > 0)) {
		if (globals.mutex) {
			switch_mutex_unlock(globals.mutex);
		}
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Received heartbeat, but we're paused - ignoring\n");
		return SWITCH_STATUS_SUCCESS;
	}

	/* Have we done any billing on this channel yet? If no, set up vars for doing so */
	if (!rednibble_data) {
		rednibble_data = switch_core_session_alloc(session, sizeof(*rednibble_data));
		memset(rednibble_data, 0, sizeof(*rednibble_data));

		/* Setup new billing data (based on call answer time, in case this module started late with active calls) */
		rednibble_data->lastts = profile->times->answered;	/* Set the initial answer time to match when the call was really answered */
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Beginning new billing on %s\n", uuid);
	}

	switch_time_exp_lt(&tm, rednibble_data->lastts);
	switch_strftime_nocheck(date, &retsize, sizeof(date), "%Y-%m-%d %T", &tm);

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%d seconds passed since last bill time of %s\n",
					  (int) ((ts - rednibble_data->lastts) / 1000000), date);

	if ((ts - rednibble_data->lastts) >= 0) {
		/* If billincrement is set we bill by it and not by time elapsed */
		if (!(switch_strlen_zero(billincrement))) {
			switch_time_t chargedunits = (ts - rednibble_data->lastts) / 1000000 <= atol(billincrement) ? atol(billincrement) * 1000000 : (switch_time_t)(ceil((ts - rednibble_data->lastts) / (atol(billincrement) * 1000000.0))) * atol(billincrement) * 1000000;
			billamount = (atof(billrate) / 1000000 / 60) * chargedunits - rednibble_data->bill_adjustments;
			/* Account for the prepaid amount */
			rednibble_data->lastts += chargedunits;
		} else {		
			/* Convert billrate into microseconds and multiply by # of microseconds that have passed since last *successful* bill */
			billamount = (atof(billrate) / 1000000 / 60) * ((ts - rednibble_data->lastts)) - rednibble_data->bill_adjustments;
			/* Update the last time we billed */
			rednibble_data->lastts = ts;
		}

		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Billing %f to %s (Call: %s / %f so far)\n", billamount, billaccount,
						  uuid, rednibble_data->total);

		/* DO BILLING HERE and reset counters if it's successful! */
		if (bill_event(billamount, billaccount, channel) == SWITCH_STATUS_SUCCESS) {
			/* Increment total cost */
			rednibble_data->total += billamount;

			/* Reset manual billing adjustments from pausing */
			rednibble_data->bill_adjustments = 0;

			/* Update channel variable with current billing */
			switch_channel_set_variable_printf(channel, "rednibble_total_billed", "%f", rednibble_data->total);
		} else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_CRIT, "Failed to log to database!\n");
		}
	} else {
		if (switch_strlen_zero(billincrement))
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Just tried to bill %s negative minutes! That should be impossible.\n", uuid);
	}

	/* Save this location */
	if (channel) {
		switch_channel_set_private(channel, "_rednibble_data_", rednibble_data);

		/* don't verify balance and transfer to nobal if we're done with call */
		if (switch_channel_get_state(channel) != CS_REPORTING && switch_channel_get_state(channel) != CS_HANGUP) {
			
			balance = get_balance(billaccount, channel);
			
			/* See if we've achieved low balance */
			if (!rednibble_data->lowbal_action_executed && balance <= lowbal_amt) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Balance of %f fell below low balance amount of %f! (Account %s)\n",
								  balance, lowbal_amt, billaccount);

				if (exec_app(session, globals.lowbal_action) != SWITCH_STATUS_SUCCESS)
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Low balance action didn't execute\n");
				else
					rednibble_data->lowbal_action_executed = 1;
			}

			/* See if this person has enough money left to continue the call */
			if (balance <= nobal_amt) {
				/* Not enough money - reroute call to nobal location */
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_CRIT, "Balance of %f fell below allowed amount of %f! (Account %s)\n",
								  balance, nobal_amt, billaccount);

				/* IMPORTANT: Billing must be paused before the transfer occurs! This prevents infinite loops, since the transfer will result */
				/* in rednibblebill checking the call again in the routing process for an allowed balance! */
				/* If you intend to give the user the option to re-up their balance, you must clear & resume billing once the balance is updated! */
				rednibblebill_pause(session);
				transfer_call(session, globals.nobal_action);
			}
		}
	}


	/* Done changing - release lock */
	if (globals.mutex) {
		switch_mutex_unlock(globals.mutex);
	}

	/* Go check if this call is allowed to continue */

	return SWITCH_STATUS_SUCCESS;
}

/* You can turn on session heartbeat on a channel to have us check billing more often */
static void event_handler(switch_event_t *event)
{
	switch_core_session_t *session;
	char *uuid;

	if (!event) {
		/* We should never get here - it means an event came in without the event info */
		return;
	}

	/* Make sure everything is sane */
	if (!(uuid = switch_event_get_header(event, "Unique-ID"))) {
		/* Donde esta channel? */
		return;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Received request via %s!\n", switch_event_name(event->event_id));

	/* Display debugging info */
	if (switch_event_get_header(event, "rednibble_debug")) {
		debug_event_handler(event);
	}

	/* Get session var */
	if (!(session = switch_core_session_locate(uuid))) {
		return;
	}

	/* Go bill */
	do_billing(session);

	switch_core_session_rwunlock(session);
}

static void rednibblebill_pause(switch_core_session_t *session)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_time_t ts = switch_micro_time_now();
	rednibble_data_t *rednibble_data;

	if (!channel) {
		return;
	}

	/* Lock this session's data for this module while we tinker with it */
	if (globals.mutex) {
		switch_mutex_lock(globals.mutex);
	}

	/* Get our rednibble data var. This will be NULL if it's our first call here for this session */
	rednibble_data = (rednibble_data_t *) switch_channel_get_private(channel, "_rednibble_data_");

	if (!rednibble_data) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Can't pause - channel is not initialized for billing!\n");
		return;
	}

	/* Set pause counter if not already set */
	if (rednibble_data->pausets == 0)
		rednibble_data->pausets = ts;

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Paused billing timestamp!\n");

	/* Done checking - release lock */
	if (globals.mutex) {
		switch_mutex_unlock(globals.mutex);
	}
}

static void rednibblebill_resume(switch_core_session_t *session)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_time_t ts = switch_micro_time_now();
	rednibble_data_t *rednibble_data;
	const char *billrate;

	if (!channel) {
		return;
	}

	/* Get our rednibble data var. This will be NULL if it's our first call here for this session */
	rednibble_data = (rednibble_data_t *) switch_channel_get_private(channel, "_rednibble_data_");

	if (!rednibble_data) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
						  "Can't resume - channel is not initialized for billing (This is expected at hangup time)!\n");
		return;
	}

	if (rednibble_data->pausets == 0) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
						  "Can't resume - channel is not paused! (This is expected at hangup time)\n");
		return;
	}

	/* Lock this session's data for this module while we tinker with it */
	if (globals.mutex) {
		switch_mutex_lock(globals.mutex);
	}

	billrate = switch_channel_get_variable(channel, "rednibble_rate");

	/* Calculate how much was "lost" to billings during pause - we do this here because you never know when the billrate may change during a call */
	rednibble_data->bill_adjustments += (atof(billrate) / 1000000 / 60) * ((ts - rednibble_data->pausets));
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Resumed billing! Subtracted %f from this billing cycle.\n",
					  (atof(billrate) / 1000000 / 60) * ((ts - rednibble_data->pausets)));

	rednibble_data->pausets = 0;

	/* Done checking - release lock */
	if (globals.mutex) {
		switch_mutex_unlock(globals.mutex);
	}
}

static void rednibblebill_reset(switch_core_session_t *session)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_time_t ts = switch_micro_time_now();
	rednibble_data_t *rednibble_data;

	if (!channel) {
		return;
	}

	/* Get our rednibble data var. This will be NULL if it's our first call here for this session */
	rednibble_data = (rednibble_data_t *) switch_channel_get_private(channel, "_rednibble_data_");

	if (!rednibble_data) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Can't reset - channel is not initialized for billing!\n");
		return;
	}

	/* Lock this session's data for this module while we tinker with it */
	if (globals.mutex) {
		switch_mutex_lock(globals.mutex);
	}

	/* Update the last time we billed */
	rednibble_data->lastts = ts;

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Reset last billing timestamp marker to right now!\n");

	/* Done checking - release lock */
	if (globals.mutex) {
		switch_mutex_unlock(globals.mutex);
	}
}

static double rednibblebill_check(switch_core_session_t *session)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	rednibble_data_t *rednibble_data;
	double amount = 0;

	if (!channel) {
		return -99999;
	}

	/* Get our rednibble data var. This will be NULL if it's our first call here for this session */
	rednibble_data = (rednibble_data_t *) switch_channel_get_private(channel, "_rednibble_data_");

	if (!rednibble_data) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Can't check - channel is not initialized for billing!\n");
		return -99999;
	}

	/* Lock this session's data for this module while we tinker with it */
	if (globals.mutex) {
		switch_mutex_lock(globals.mutex);
	}

	amount = rednibble_data->total;

	/* Done checking - release lock */
	if (globals.mutex) {
		switch_mutex_unlock(globals.mutex);
	}

	return amount;
}

static void rednibblebill_adjust(switch_core_session_t *session, double amount)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	const char *billaccount;

	if (!channel) {
		return;
	}

	/* Variables kept in FS but relevant only to this module */

	billaccount = switch_channel_get_variable(channel, "rednibble_account");

	/* Return if there's no billing information on this session */
	if (!billaccount) {
		return;
	}

	/* Add or remove amount from adjusted billing here. Note, we bill the OPPOSITE */
	if (bill_event(-amount, billaccount, channel) == SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Recorded adjustment to %s for %f\n", billaccount, amount);
	} else {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Failed to record adjustment to %s for %f\n", billaccount, amount);
	}
}

#define APP_SYNTAX "pause | resume | reset | adjust <amount> | heartbeat <seconds> | check"
SWITCH_STANDARD_APP(rednibblebill_app_function)
{
	int argc = 0;
	char *lbuf = NULL;
	char *argv[3] = { 0 };

	if (!zstr(data) && (lbuf = strdup(data))
		&& (argc = switch_separate_string(lbuf, ' ', argv, (sizeof(argv) / sizeof(argv[0]))))) {
		if (!strcasecmp(argv[0], "adjust") && argc == 2) {
			rednibblebill_adjust(session, atof(argv[1]));
		} else if (!strcasecmp(argv[0], "flush")) {
			do_billing(session);
		} else if (!strcasecmp(argv[0], "pause")) {
			rednibblebill_pause(session);
		} else if (!strcasecmp(argv[0], "resume")) {
			rednibblebill_resume(session);
		} else if (!strcasecmp(argv[0], "check")) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Current billing is at %f\n", rednibblebill_check(session));
		} else if (!strcasecmp(argv[0], "reset")) {
			rednibblebill_reset(session);
		} else if (!strcasecmp(argv[0], "heartbeat") && argc == 2) {
			switch_core_session_enable_heartbeat(session, atoi(argv[1]));
		}
	}
	switch_safe_free(lbuf);
}

/* We get here from the API only (theoretically) */
#define API_SYNTAX "<uuid> [pause | resume | reset | adjust <amount> | heartbeat <seconds> | check]"
SWITCH_STANDARD_API(rednibblebill_api_function)
{
	switch_core_session_t *psession = NULL;
	char *mycmd = NULL, *argv[3] = { 0 };
	int argc = 0;

	if (!zstr(cmd) && (mycmd = strdup(cmd))) {
		argc = switch_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
		if ((argc == 2 || argc == 3) && !zstr(argv[0])) {
			char *uuid = argv[0];
			if ((psession = switch_core_session_locate(uuid))) {
				if (!strcasecmp(argv[1], "adjust") && argc == 3) {
					rednibblebill_adjust(psession, atof(argv[2]));
				} else if (!strcasecmp(argv[1], "flush")) {
					do_billing(psession);
				} else if (!strcasecmp(argv[1], "pause")) {
					rednibblebill_pause(psession);
				} else if (!strcasecmp(argv[1], "resume")) {
					rednibblebill_resume(psession);
				} else if (!strcasecmp(argv[1], "check")) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Current billing is at %f\n", rednibblebill_check(psession));
				} else if (!strcasecmp(argv[1], "reset")) {
					rednibblebill_reset(psession);
				} else if (!strcasecmp(argv[1], "heartbeat") && argc == 3) {
					switch_core_session_enable_heartbeat(psession, atoi(argv[2]));
				}

				switch_core_session_rwunlock(psession);
			} else {
				stream->write_function(stream, "-ERR No Such Channel!\n");
			}
		} else {
			stream->write_function(stream, "-USAGE: %s\n", API_SYNTAX);
		}
	}
	switch_safe_free(mycmd);
	return SWITCH_STATUS_SUCCESS;
}

/* Check if session has variable "billrate" set. If it does, activate the heartbeat variable
 switch_core_session_enable_heartbeat(switch_core_session_t *session, uint32_t seconds)
 switch_core_session_sched_heartbeat(switch_core_session_t *session, uint32_t seconds)*/

static switch_status_t sched_billing(switch_core_session_t *session)
{
	switch_channel_t *channel = NULL;
	
	const char *billrate = NULL;
	const char *billaccount = NULL;
	
	if (!(channel = switch_core_session_get_channel(session))) {
		return SWITCH_STATUS_SUCCESS;
	}

	/* Variables kept in FS but relevant only to this module */
	billrate = switch_channel_get_variable(channel, "rednibble_rate");
	billaccount = switch_channel_get_variable(channel, "rednibble_account");
	
	/* Return if there's no billing information on this session */
	if (!billrate || !billaccount) {
		return SWITCH_STATUS_SUCCESS;
	}

	if (globals.global_heartbeat > 0) {
		switch_core_session_enable_heartbeat(session, globals.global_heartbeat);
	}

	/* TODO: Check account balance here */

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t process_hangup(switch_core_session_t *session)
{
	const char* billaccount;
	switch_channel_t *channel = NULL;

	channel = switch_core_session_get_channel(session);
	
	/* Resume any paused billings, just in case */
	/*  rednibblebill_resume(session); */

	/* Now go handle like normal billing */
	do_billing(session);

	billaccount = switch_channel_get_variable(channel, "rednibble_account");
	if (billaccount) {
		switch_channel_set_variable_printf(channel, "rednibble_current_balance", "%f", get_balance(billaccount, channel));
	}			
	
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t process_and_sched(switch_core_session_t *session) {
	process_hangup(session);
	sched_billing(session);
	return SWITCH_STATUS_SUCCESS;
}

switch_state_handler_table_t rednibble_state_handler = {
	/* on_init */ NULL,
	/* on_routing */ process_hangup, 	/* Need to add a check here for anything in their account before routing */
	/* on_execute */ sched_billing, 	/* Turn on heartbeat for this session and do an initial account check */
	/* on_hangup */ process_hangup, 	/* On hangup - most important place to go bill */
	/* on_exch_media */ process_and_sched,
	/* on_soft_exec */ NULL,
	/* on_consume_med */ process_and_sched,
	/* on_hibernate */ NULL,
	/* on_reset */ NULL,
	/* on_park */ NULL,
	/* on_reporting */ NULL, 
	/* on_destroy */ NULL
};

SWITCH_MODULE_LOAD_FUNCTION(mod_rednibblebill_load)
{
	switch_api_interface_t *api_interface;
	switch_application_interface_t *app_interface;
	REDIS redis;

	/* Set every byte in this structure to 0 */
	memset(&globals, 0, sizeof(globals));
	globals.pool = pool;
	switch_mutex_init(&globals.mutex, SWITCH_MUTEX_NESTED, globals.pool);

	load_config();

	/* connect my internal structure to the blank pointer passed to me */
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	/* Add API and CLI commands */
	SWITCH_ADD_API(api_interface, "rednibblebill", "Manage billing parameters for a channel/call", rednibblebill_api_function, API_SYNTAX);

	/* Add dialplan applications */
	SWITCH_ADD_APP(app_interface, "rednibblebill", "Handle billing for the current channel/call",
				   "Pause, resume, reset, adjust, flush, heartbeat commands to handle billing.", rednibblebill_app_function, APP_SYNTAX,
				   SAF_SUPPORT_NOMEDIA | SAF_ROUTING_EXEC);

	/* register state handlers for billing */
	switch_core_add_state_handler(&rednibble_state_handler);

	/* bind to heartbeat events */
	if (switch_event_bind_removable(modname, SWITCH_EVENT_SESSION_HEARTBEAT, SWITCH_EVENT_SUBCLASS_ANY, event_handler, NULL, &globals.node) !=
		SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't bind event to monitor for session heartbeats!\n");
		return SWITCH_STATUS_GENERR;
	}

	if (redis_factory(&redis) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	/* indicate that the module should continue to be loaded */
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_rednibblebill_shutdown)
{
	switch_event_unbind(&globals.node);
	switch_core_remove_state_handler(&rednibble_state_handler);
	

	switch_safe_free(globals.redis_host);
	switch_safe_free(globals.percall_action);
	switch_safe_free(globals.lowbal_action);
	switch_safe_free(globals.nobal_action);

	return SWITCH_STATUS_UNLOAD;
}
