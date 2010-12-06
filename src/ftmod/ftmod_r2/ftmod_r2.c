/*
 * Copyright (c) 2009, Moises Silva <moy@sangoma.com>
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 
 * * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 
 * * Neither the name of the original author; nor the names of any contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 * 
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef __linux__
#ifndef _BSD_SOURCE
#define _BSD_SOURCE /* for strsep() */
#endif
#include <syscall.h>
#include <poll.h>
#include <string.h>
#endif
#include <stdio.h>
#include <openr2.h>
#include "freetdm.h"
#include "private/ftdm_core.h"

/* debug thread count for r2 legs */
static ftdm_mutex_t* g_thread_count_mutex;
static int32_t g_thread_count = 0;

typedef int openr2_call_status_t;

/* when the users kills a span we clear this flag to kill the signaling thread */
/* FIXME: what about the calls that are already up-and-running? */
typedef enum {
	FTDM_R2_RUNNING = (1 << 0),
} ftdm_r2_flag_t;

/* private call information stored in ftdmchan->call_data void* ptr */
#define R2CALL(ftdmchan) ((ftdm_r2_call_t*)((ftdmchan)->call_data))
typedef struct ftdm_r2_call_t {
	openr2_chan_t *r2chan;
	int accepted:1;
	int answer_pending:1;
	int disconnect_rcvd:1;
	int ftdm_call_started:1;
	int protocol_error:1;
	ftdm_channel_state_t chanstate;
	ftdm_size_t dnis_index;
	ftdm_size_t ani_index;
	char logname[255];
	char name[10];
	unsigned long txdrops;
} ftdm_r2_call_t;

/* this is just used as place holder in the stack when configuring the span to avoid using bunch of locals */
typedef struct ft_r2_conf_s {
	/* openr2 types */
	openr2_variant_t variant;
	openr2_calling_party_category_t category;
	openr2_log_level_t loglevel;

	/* strings */
	char *logdir;
	char *advanced_protocol_file;

	/* ints */
	int32_t max_ani;
	int32_t max_dnis;
	int32_t mfback_timeout; 
	int32_t metering_pulse_timeout;
	int32_t mf_dump_size;

	/* booleans */
	int immediate_accept;
	int skip_category;
	int get_ani_first;
	int call_files;
	int double_answer;
	int charge_calls;
	int forced_release;
	int allow_collect_calls;
} ft_r2_conf_t;

/* r2 configuration stored in span->signal_data */
typedef struct ftdm_r2_data_s {
	/* span flags */
	ftdm_r2_flag_t flags;
	/* openr2 handle for the R2 variant context */
	openr2_context_t *r2context;
	/* category to use when making calls */
	openr2_calling_party_category_t category;
	/* whether to use OR2_CALL_WITH_CHARGE or OR2_CALL_NO_CHARGE when accepting a call */
	int charge_calls:1;
	/* allow or reject collect calls */
	int allow_collect_calls:1;
	/* whether to use forced release when hanging up */
	int forced_release:1;
	/* whether accept the call when offered, or wait until the user decides to accept */
	int accept_on_offer:1;
	/* Size of multi-frequency (or any media) dumps used during protocol errors */
	int32_t mf_dump_size;
	/* max time spent in ms doing real work in a single loop */
	int32_t jobmax;
	/* Total number of loops performed so far */
	uint64_t total_loops;
	/* number of loops per 10ms increment from 0-9ms, 10-19ms .. 100ms and above */
	uint64_t loops[11];
	/* LWP */
	uint32_t monitor_thread_id;
	/* Logging directory */
	char logdir[512];
} ftdm_r2_data_t;

/* one element per span will be stored in g_mod_data_hash global var to keep track of them
   and destroy them on module unload */
typedef struct ftdm_r2_span_pvt_s {
	openr2_context_t *r2context; /* r2 context allocated for this span */
	ftdm_hash_t *r2calls; /* hash table of allocated call data per channel for this span */
} ftdm_r2_span_pvt_t;

/* span monitor thread */
static void *ftdm_r2_run(ftdm_thread_t *me, void *obj);

/* hash of all the private span allocations
   we need to keep track of them to destroy them when unloading the module 
   since freetdm does not notify signaling modules when destroying a span
   span -> ftdm_r2_mod_allocs_t */
static ftdm_hash_t *g_mod_data_hash;

/* IO interface for the command API */
static ftdm_io_interface_t g_ftdm_r2_interface;

static int ftdm_r2_state_advance(ftdm_channel_t *ftdmchan);
static void ftdm_r2_state_advance_all(ftdm_channel_t *ftdmchan);

/* whether R2 call accept process is pending */
#define IS_ACCEPTING_PENDING(ftdmchan) \
		( (!ftdm_test_flag((ftdmchan), FTDM_CHANNEL_OUTBOUND)) && !R2CALL((ftdmchan))->accepted && \
				((ftdmchan)->state == FTDM_CHANNEL_STATE_PROGRESS || \
				 (ftdmchan)->state == FTDM_CHANNEL_STATE_PROGRESS_MEDIA || \
				 (ftdmchan)->state == FTDM_CHANNEL_STATE_UP) )

/* functions not available on windows */
#ifdef WIN32
#include <mmsystem.h>

static __inline int gettimeofday(struct timeval *tp, void *nothing)
{
#ifdef WITHOUT_MM_LIB
    SYSTEMTIME st;
    time_t tt;
    struct tm tmtm;
    /* mktime converts local to UTC */
    GetLocalTime (&st);
    tmtm.tm_sec = st.wSecond;
    tmtm.tm_min = st.wMinute;
    tmtm.tm_hour = st.wHour;
    tmtm.tm_mday = st.wDay;
    tmtm.tm_mon = st.wMonth - 1;
    tmtm.tm_year = st.wYear - 1900;  tmtm.tm_isdst = -1;
    tt = mktime (&tmtm);
    tp->tv_sec = tt;
    tp->tv_usec = st.wMilliseconds * 1000;
#else
    /**
     ** The earlier time calculations using GetLocalTime
     ** had a time resolution of 10ms.The timeGetTime, part
     ** of multimedia apis offer a better time resolution
     ** of 1ms.Need to link against winmm.lib for this
     **/
    unsigned long Ticks = 0;
    unsigned long Sec =0;
    unsigned long Usec = 0;
    Ticks = timeGetTime();

    Sec = Ticks/1000;
    Usec = (Ticks - (Sec*1000))*1000;
    tp->tv_sec = Sec;
    tp->tv_usec = Usec;
#endif /* WITHOUT_MM_LIB */
    (void)nothing;
    return 0;
}

static char *strsep(char **stringp, const char *delim)
{
   char *start = *stringp;
   char *ptr;

   if (!start)
       return NULL;

   if (!*delim)
       ptr = start + strlen(start);
   else {
       ptr = strpbrk(start, delim);
       if (!ptr) {
           *stringp = NULL;
           return start;
       }
   }

   *ptr = '\0';
   *stringp = ptr + 1;

   return start;
}
#endif /* WIN32 */

static void ftdm_r2_set_chan_sig_status(ftdm_channel_t *ftdmchan, ftdm_signaling_status_t status)
{
	ftdm_sigmsg_t sig;
	ftdm_log_chan(ftdmchan, FTDM_LOG_DEBUG, "Signalling link status changed to %s\n", ftdm_signaling_status2str(status));

	memset(&sig, 0, sizeof(sig));
	sig.chan_id = ftdmchan->chan_id;
	sig.span_id = ftdmchan->span_id;
	sig.channel = ftdmchan;
	sig.event_id = FTDM_SIGEVENT_SIGSTATUS_CHANGED;
	sig.sigstatus = status;
	if (ftdm_span_send_signal(ftdmchan->span, &sig) != FTDM_SUCCESS) {
		ftdm_log_chan(ftdmchan, FTDM_LOG_ERROR, "Failed to change channel status to %s\n", ftdm_signaling_status2str(status));
	}
	return;
}

static ftdm_call_cause_t ftdm_r2_cause_to_ftdm_cause(ftdm_channel_t *fchan, openr2_call_disconnect_cause_t cause)
{
	switch (cause) {

	case OR2_CAUSE_NORMAL_CLEARING:
		return FTDM_CAUSE_NORMAL_CLEARING;

	case OR2_CAUSE_BUSY_NUMBER:
		return FTDM_CAUSE_USER_BUSY;

	case OR2_CAUSE_NETWORK_CONGESTION:
		return FTDM_CAUSE_SWITCH_CONGESTION;

	case OR2_CAUSE_UNALLOCATED_NUMBER:
		return FTDM_CAUSE_NO_ROUTE_DESTINATION;

	case OR2_CAUSE_NUMBER_CHANGED:
		return FTDM_CAUSE_NUMBER_CHANGED;

	case OR2_CAUSE_OUT_OF_ORDER:
		return FTDM_CAUSE_NETWORK_OUT_OF_ORDER;

	case OR2_CAUSE_NO_ANSWER:
		return FTDM_CAUSE_NO_ANSWER;
	
	case OR2_CAUSE_UNSPECIFIED:
		return FTDM_CAUSE_NORMAL_UNSPECIFIED;

	case OR2_CAUSE_COLLECT_CALL_REJECTED:
		return FTDM_CAUSE_CALL_REJECTED;

	case OR2_CAUSE_FORCED_RELEASE:
		return FTDM_CAUSE_NORMAL_CLEARING;
	}
	ftdm_log_chan(fchan, FTDM_LOG_WARNING, "Mapping openr2 cause %d to unspecified\n", cause);
	return FTDM_CAUSE_NORMAL_UNSPECIFIED;
}

static openr2_call_disconnect_cause_t ftdm_r2_ftdm_cause_to_openr2_cause(ftdm_channel_t *fchan)
{
	switch (fchan->caller_data.hangup_cause) {

	case FTDM_CAUSE_NORMAL_CLEARING:
		return OR2_CAUSE_NORMAL_CLEARING;

	case FTDM_CAUSE_USER_BUSY:
		return OR2_CAUSE_BUSY_NUMBER;

	case FTDM_CAUSE_SWITCH_CONGESTION:
		return OR2_CAUSE_NETWORK_CONGESTION;

	case FTDM_CAUSE_NO_ROUTE_DESTINATION:
		return OR2_CAUSE_UNALLOCATED_NUMBER;

	case FTDM_CAUSE_NUMBER_CHANGED:
		return OR2_CAUSE_NUMBER_CHANGED;

	case FTDM_CAUSE_NETWORK_OUT_OF_ORDER:
	case FTDM_CAUSE_SERVICE_UNAVAILABLE:
		return OR2_CAUSE_OUT_OF_ORDER;

	case FTDM_CAUSE_NO_ANSWER:
	case FTDM_CAUSE_NO_USER_RESPONSE:
		return OR2_CAUSE_NO_ANSWER;
	
	case FTDM_CAUSE_NORMAL_UNSPECIFIED:
		return OR2_CAUSE_UNSPECIFIED;

	}
	ftdm_log_chan(fchan, FTDM_LOG_WARNING, "freetdm hangup cause %d mapped to openr2 cause %s\n",
			fchan->caller_data.hangup_cause, openr2_proto_get_disconnect_string(OR2_CAUSE_UNSPECIFIED));
	return OR2_CAUSE_UNSPECIFIED;
}

static void ft_r2_clean_call(ftdm_r2_call_t *call)
{
	openr2_chan_t *r2chan = call->r2chan;
	memset(call, 0, sizeof(*call));
	call->r2chan = r2chan;
}

static void ft_r2_accept_call(ftdm_channel_t *ftdmchan)
{
	openr2_chan_t *r2chan = R2CALL(ftdmchan)->r2chan;
	// FIXME: not always accept as no charge, let the user decide that
	// also we should check the return code from openr2_chan_accept_call and handle error condition
	// hanging up the call with protocol error as the reason, this openr2 API will fail only when there something
	// wrong at the I/O layer or the library itself
	openr2_chan_accept_call(r2chan, OR2_CALL_NO_CHARGE);
}

static void ft_r2_answer_call(ftdm_channel_t *ftdmchan)
{
	openr2_chan_t *r2chan = R2CALL(ftdmchan)->r2chan;
	// FIXME
	// 1. check openr2_chan_answer_call return code
	// 2. The openr2_chan_answer_call_with_mode should be used depending on user settings
	// openr2_chan_answer_call_with_mode(r2chan, OR2_ANSWER_SIMPLE);
	openr2_chan_answer_call(r2chan);
	R2CALL(ftdmchan)->answer_pending = 0;
}

/* this function must be called with the chan mutex held! */
static FIO_CHANNEL_OUTGOING_CALL_FUNCTION(r2_outgoing_call)
{
	openr2_call_status_t callstatus;
	ftdm_r2_data_t *r2data;

	r2data = ftdmchan->span->signal_data;

	if (ftdmchan->state != FTDM_CHANNEL_STATE_DOWN) {
		/* collision, an incoming seized the channel between our take and use timing */
		ftdm_log_chan(ftdmchan, 
		FTDM_LOG_CRIT, "R2 cannot dial out in channel in state %s, try another channel!.\n", ftdm_channel_state2str(ftdmchan->state));
		return FTDM_FAIL;
	}

	ft_r2_clean_call(ftdmchan->call_data);
	R2CALL(ftdmchan)->ftdm_call_started = 1;
	R2CALL(ftdmchan)->chanstate = FTDM_CHANNEL_STATE_DOWN;
	ftdm_set_state(ftdmchan, FTDM_CHANNEL_STATE_DIALING);

	callstatus = openr2_chan_make_call(R2CALL(ftdmchan)->r2chan, 
			ftdmchan->caller_data.cid_num.digits, 
			ftdmchan->caller_data.dnis.digits, 
			OR2_CALLING_PARTY_CATEGORY_NATIONAL_SUBSCRIBER);

	if (callstatus) {
		ftdm_log_chan_msg(ftdmchan, FTDM_LOG_CRIT, "Failed to make call in R2 channel, openr2_chan_make_call failed\n");
		return FTDM_FAIL;
	}

	if (ftdmchan->state !=  FTDM_CHANNEL_STATE_DIALING) {
		ftdm_log_chan(ftdmchan, FTDM_LOG_WARNING, "Collision after call attempt, try another channel, new state = %s\n",
				ftdm_channel_state2str(ftdmchan->state));
		return FTDM_BREAK;
	}
	return FTDM_SUCCESS;
}

static ftdm_status_t ftdm_r2_start(ftdm_span_t *span)
{
	ftdm_r2_data_t *r2_data = span->signal_data;
	ftdm_set_flag(r2_data, FTDM_R2_RUNNING);
	return ftdm_thread_create_detached(ftdm_r2_run, span);
}

static ftdm_status_t ftdm_r2_stop(ftdm_span_t *span)
{
	ftdm_r2_data_t *r2_data = span->signal_data;
	while (ftdm_test_flag(r2_data, FTDM_R2_RUNNING)) {
		ftdm_log(FTDM_LOG_DEBUG, "Waiting for R2 span %s\n", span->name);
		ftdm_sleep(100);
	}
	return FTDM_SUCCESS;
}

static FIO_CHANNEL_GET_SIG_STATUS_FUNCTION(ftdm_r2_get_channel_sig_status)
{
	if (ftdm_test_flag(ftdmchan, FTDM_CHANNEL_SIG_UP)) {
		*status = FTDM_SIG_STATE_UP;
	} else {
		*status = FTDM_SIG_STATE_DOWN;
	}

	return FTDM_SUCCESS;
}

/* always called from the monitor thread */
static void ftdm_r2_on_call_init(openr2_chan_t *r2chan, const char *logname)
{
	ftdm_r2_call_t *r2call;
	ftdm_channel_t *ftdmchan = openr2_chan_get_client_data(r2chan);
	ftdm_r2_data_t *r2data = ftdmchan->span->signal_data;

	ftdm_log_chan_msg(ftdmchan, FTDM_LOG_NOTICE, "Received request to start call\n");

	if (ftdm_test_flag(ftdmchan, FTDM_CHANNEL_INUSE)) {
		ftdm_log_chan(ftdmchan, FTDM_LOG_CRIT, "Cannot start call when channel is in use (state = %s)\n", ftdm_channel_state2str(ftdmchan->state));
		return;
	}

	if (ftdmchan->state != FTDM_CHANNEL_STATE_DOWN) {
		ftdm_log_chan(ftdmchan, FTDM_LOG_ERROR, "Cannot handle request to start call in state %s\n", ftdm_channel_state2str(ftdmchan->state));
		return;
	}

	if (ftdm_channel_open_chan(ftdmchan) != FTDM_SUCCESS) {
		ftdm_log_chan(ftdmchan, FTDM_LOG_ERROR, "Failed to open channel during incoming call! [%s]\n", ftdmchan->last_error);
		return;
	}

	/* mark the channel in use (so no outgoing calls can be placed here) */
	ftdm_channel_use(ftdmchan);	

	memset(ftdmchan->caller_data.dnis.digits, 0, sizeof(ftdmchan->caller_data.collected));
	memset(ftdmchan->caller_data.ani.digits, 0, sizeof(ftdmchan->caller_data.collected));

	/* clean the call data structure but keep the R2 processing flag on! */
	ft_r2_clean_call(ftdmchan->call_data);
	r2call = R2CALL(ftdmchan);

	snprintf(r2call->logname, sizeof(r2call->logname), "%s", logname);

	/* start io dump */
	if (r2data->mf_dump_size) {
		ftdm_channel_command(ftdmchan, FTDM_COMMAND_ENABLE_INPUT_DUMP, &r2data->mf_dump_size);
		ftdm_channel_command(ftdmchan, FTDM_COMMAND_ENABLE_OUTPUT_DUMP, &r2data->mf_dump_size);
	}

	R2CALL(ftdmchan)->chanstate = FTDM_CHANNEL_STATE_DOWN;
	ftdm_set_state(ftdmchan, FTDM_CHANNEL_STATE_COLLECT);
}

/* only called for incoming calls when the ANI, DNIS etc is complete and the user has to decide either to accept or reject the call */
static void ftdm_r2_on_call_offered(openr2_chan_t *r2chan, const char *ani, const char *dnis, openr2_calling_party_category_t category)
{
	ftdm_channel_t *ftdmchan = openr2_chan_get_client_data(r2chan);
	ftdm_r2_data_t *r2data = ftdmchan->span->signal_data;

	ftdm_log_chan(ftdmchan, FTDM_LOG_NOTICE, "Call offered with ANI = %s, DNIS = %s, Category = (%d)\n", ani, dnis, category);
	ftdm_set_state(ftdmchan, FTDM_CHANNEL_STATE_RING);

	/* nothing went wrong during call setup, MF has ended, we can and must disable the MF dump */
	if (r2data->mf_dump_size) {
		ftdm_channel_command(ftdmchan, FTDM_COMMAND_DISABLE_INPUT_DUMP, NULL);
		ftdm_channel_command(ftdmchan, FTDM_COMMAND_DISABLE_OUTPUT_DUMP, NULL);
	}
}

/*
 * Accepting a call in R2 is a lengthy process due to MF tones,
 * when the user sends PROGRESS indication (implicitly moving the
 * ftdm channel to PROGRESS state) the R2 processing loop
 * does not clear FTDM_CHANNEL_STATE_CHANGE immediately as it does
 * for all the other states, instead has to wait for on_call_accepted
 * callback from openr2, which means the MF has ended and the progress
 * indication is done, in order to clear the flag. However, if
 * a protocol error or call disconnection (which is indicated using CAS bits)
 * occurrs while accepting, we must clear the pending flag, this function
 * takes care of that
 * */
static void clear_accept_pending(ftdm_channel_t *fchan)
{
	if (IS_ACCEPTING_PENDING(fchan)) {
		ftdm_clear_flag(fchan, FTDM_CHANNEL_STATE_CHANGE);
		ftdm_channel_complete_state(fchan);
	} else if (ftdm_test_flag(fchan, FTDM_CHANNEL_STATE_CHANGE)) {
		ftdm_log_chan(fchan, FTDM_LOG_CRIT, "State change flag set in state %s, last state = %s\n", 
				ftdm_channel_state2str(fchan->state), ftdm_channel_state2str(fchan->last_state));
		ftdm_clear_flag(fchan, FTDM_CHANNEL_STATE_CHANGE);
		ftdm_channel_complete_state(fchan);
	}
}

static void dump_mf(openr2_chan_t *r2chan)
{
	char dfile[512];
	FILE *f = NULL;
	ftdm_channel_t *ftdmchan = openr2_chan_get_client_data(r2chan);
	ftdm_r2_data_t *r2data = ftdmchan->span->signal_data;
	if (r2data->mf_dump_size) {
		char *logname = R2CALL(ftdmchan)->logname;
		
		ftdm_log_chan(ftdmchan, FTDM_LOG_ERROR, "Dumping IO output in prefix %s\n", logname);
		snprintf(dfile, sizeof(dfile), logname ? "%s.s%dc%d.input.alaw" : "%s/s%dc%d.input.alaw", 
				logname ? logname : r2data->logdir, ftdmchan->span_id, ftdmchan->chan_id);
		f = fopen(dfile, "w");
		ftdm_log_chan(ftdmchan, FTDM_LOG_ERROR, "Dumping IO input in file %s\n", dfile);
		ftdm_channel_command(ftdmchan, FTDM_COMMAND_DUMP_INPUT, f);
		fclose(f);

		snprintf(dfile, sizeof(dfile), logname ? "%s.s%dc%d.output.alaw" : "%s/s%dc%d.output.alaw", 
				logname ? logname : r2data->logdir, ftdmchan->span_id, ftdmchan->chan_id);
		f = fopen(dfile, "w");
		ftdm_log_chan(ftdmchan, FTDM_LOG_ERROR, "Dumping IO output in file %s\n", dfile);
		ftdm_channel_command(ftdmchan, FTDM_COMMAND_DUMP_OUTPUT, f);
		fclose(f);
	}
}

static void ftdm_r2_on_call_accepted(openr2_chan_t *r2chan, openr2_call_mode_t mode)
{
	ftdm_channel_t *ftdmchan = openr2_chan_get_client_data(r2chan);
	ftdm_log_chan_msg(ftdmchan, FTDM_LOG_NOTICE, "Call accepted\n");

	clear_accept_pending(ftdmchan);

	/* at this point the MF signaling has ended and there is no point on keep reading */
	openr2_chan_disable_read(r2chan);
	
	R2CALL(ftdmchan)->accepted = 1;

	if (OR2_DIR_BACKWARD == openr2_chan_get_direction(r2chan)) {
		if (R2CALL(ftdmchan)->answer_pending) {
			ftdm_log_chan_msg(ftdmchan, FTDM_LOG_DEBUG, "Answer was pending, answering now.\n");
			ft_r2_answer_call(ftdmchan);
			R2CALL(ftdmchan)->answer_pending = 0;
			return;
		}
	} else {
		ftdm_set_state(ftdmchan, FTDM_CHANNEL_STATE_PROGRESS_MEDIA);
	}
}

static void ftdm_r2_on_call_answered(openr2_chan_t *r2chan)
{
	ftdm_channel_t *ftdmchan = openr2_chan_get_client_data(r2chan);
	ftdm_log_chan_msg(ftdmchan, FTDM_LOG_NOTICE, "Call answered\n");
	/* notify the upper layer of progress in the outbound call */
	if (OR2_DIR_FORWARD == openr2_chan_get_direction(r2chan)) {
		ftdm_set_state(ftdmchan, FTDM_CHANNEL_STATE_UP);
	}
}

/* may be called in the signaling or media thread depending on whether the hangup is product of MF or CAS signaling */
static void ftdm_r2_on_call_disconnect(openr2_chan_t *r2chan, openr2_call_disconnect_cause_t cause)
{
	ftdm_channel_t *ftdmchan = openr2_chan_get_client_data(r2chan);

	ftdm_log_chan_msg(ftdmchan, FTDM_LOG_NOTICE, "Call disconnected\n");

	clear_accept_pending(ftdmchan);

	R2CALL(ftdmchan)->disconnect_rcvd = 1;

	if (ftdmchan->state == FTDM_CHANNEL_STATE_HANGUP) {
		ftdm_log_chan_msg(ftdmchan, FTDM_LOG_DEBUG, "Call had been disconnected already by the user\n");
		/* just ack the hangup to trigger the on_call_end callback and go down */
		openr2_chan_disconnect_call(r2chan, OR2_CAUSE_NORMAL_CLEARING);
		return;
	}

	ftdmchan->caller_data.hangup_cause  = ftdm_r2_cause_to_ftdm_cause(ftdmchan, cause);
	ftdm_set_state(ftdmchan, FTDM_CHANNEL_STATE_TERMINATING);
}

static void ftdm_r2_on_call_end(openr2_chan_t *r2chan)
{
	ftdm_channel_t *ftdmchan = openr2_chan_get_client_data(r2chan);
	ftdm_log_chan_msg(ftdmchan, FTDM_LOG_NOTICE, "Call finished\n");

	/* the call is done as far as the stack is concerned, lets move to down here */
	ftdm_set_state(ftdmchan, FTDM_CHANNEL_STATE_DOWN);

	/* in some circumstances openr2 can call on_call_init right after this, so let's advance the state right here */
	ftdm_r2_state_advance_all(ftdmchan);
}

static void ftdm_r2_on_call_read(openr2_chan_t *r2chan, const unsigned char *buf, int buflen)
{
#if 0
	ftdm_log(FTDM_LOG_NOTICE, "Call read data on chan %d\n", openr2_chan_get_number(r2chan));
#endif
}

static void ftdm_r2_on_hardware_alarm(openr2_chan_t *r2chan, int alarm)
{
	ftdm_channel_t *ftdmchan = openr2_chan_get_client_data(r2chan);
	ftdm_log_chan(ftdmchan, FTDM_LOG_WARNING, "Alarm notification: %d\n", alarm);
}

static void ftdm_r2_on_os_error(openr2_chan_t *r2chan, int errorcode)
{
	ftdm_channel_t *ftdmchan = openr2_chan_get_client_data(r2chan);
	ftdm_log_chan(ftdmchan, FTDM_LOG_ERROR, "OS error: %s\n", strerror(errorcode));
}

static void ftdm_r2_on_protocol_error(openr2_chan_t *r2chan, openr2_protocol_error_t reason)
{
	ftdm_channel_t *ftdmchan = openr2_chan_get_client_data(r2chan);

	if (ftdmchan->state == FTDM_CHANNEL_STATE_DOWN) {
		ftdm_log_chan_msg(ftdmchan, FTDM_LOG_ERROR, "Got protocol error when we're already down!\n");
		return;
	}

	dump_mf(r2chan);

	clear_accept_pending(ftdmchan);

	R2CALL(ftdmchan)->disconnect_rcvd = 1;
	R2CALL(ftdmchan)->protocol_error = 1;

	if (ftdmchan->state == FTDM_CHANNEL_STATE_HANGUP) {
		ftdm_log_chan_msg(ftdmchan, FTDM_LOG_ERROR, "The user already hung up, finishing call in protocol error\n");
		ftdm_set_state(ftdmchan, FTDM_CHANNEL_STATE_DOWN);
		return;
	}

	ftdmchan->caller_data.hangup_cause  = FTDM_CAUSE_PROTOCOL_ERROR; 
	ftdm_set_state(ftdmchan, FTDM_CHANNEL_STATE_TERMINATING);
}

static void ftdm_r2_on_line_blocked(openr2_chan_t *r2chan)
{
	ftdm_channel_t *ftdmchan = openr2_chan_get_client_data(r2chan);
	ftdm_log_chan(ftdmchan, FTDM_LOG_NOTICE, "Far end blocked in state %s\n", ftdm_channel_state2str(ftdmchan->state));
	ftdm_r2_set_chan_sig_status(ftdmchan, FTDM_SIG_STATE_SUSPENDED);
}

static void ftdm_r2_on_line_idle(openr2_chan_t *r2chan)
{
	ftdm_channel_t *ftdmchan = openr2_chan_get_client_data(r2chan);
	ftdm_log_chan(ftdmchan, FTDM_LOG_NOTICE, "Far end unblocked in state %s\n", ftdm_channel_state2str(ftdmchan->state));
	ftdm_r2_set_chan_sig_status(ftdmchan, FTDM_SIG_STATE_UP);
}

static void ftdm_r2_write_log(openr2_log_level_t level, const char *file, const char *function, int line, const char *message)
{
	switch (level) {
		case OR2_LOG_NOTICE:
			ftdm_log(file, function, line, FTDM_LOG_LEVEL_NOTICE, "%s", message);
			break;
		case OR2_LOG_WARNING:
			ftdm_log(file, function, line, FTDM_LOG_LEVEL_WARNING, "%s", message);
			break;
		case OR2_LOG_ERROR:
			ftdm_log(file, function, line, FTDM_LOG_LEVEL_ERROR, "%s", message);
			break;
		case OR2_LOG_STACK_TRACE:
		case OR2_LOG_MF_TRACE:
		case OR2_LOG_CAS_TRACE:
		case OR2_LOG_DEBUG:
		case OR2_LOG_EX_DEBUG:
			ftdm_log(file, function, line, FTDM_LOG_LEVEL_DEBUG, "%s", message);
			break;
		default:
			ftdm_log(FTDM_LOG_WARNING, "We should handle logging level %d here.\n", level);
			ftdm_log(file, function, line, FTDM_LOG_LEVEL_DEBUG, "%s", message);
			break;
	}
}

static void ftdm_r2_on_context_log(openr2_context_t *r2context, const char *file, const char *function, unsigned int line, 
	openr2_log_level_t level, const char *fmt, va_list ap)
{
#define CONTEXT_TAG "Context -"
	char logmsg[256];
	char completemsg[sizeof(logmsg) + sizeof(CONTEXT_TAG) - 1];
	vsnprintf(logmsg, sizeof(logmsg), fmt, ap);
	snprintf(completemsg, sizeof(completemsg), CONTEXT_TAG "%s", logmsg);
	ftdm_r2_write_log(level, file, function, line, completemsg);
#undef CONTEXT_TAG
}

static void ftdm_r2_on_chan_log(openr2_chan_t *r2chan, const char *file, const char *function, unsigned int line, 
	openr2_log_level_t level, const char *fmt, va_list ap)
{
	ftdm_channel_t *ftdmchan = openr2_chan_get_client_data(r2chan);
	char logmsg[1024];
	char completemsg[sizeof(logmsg)];
	vsnprintf(logmsg, sizeof(logmsg), fmt, ap);
	snprintf(completemsg, sizeof(completemsg), "[s%dc%d] [%d:%d] [%s] %s", 
			ftdmchan->span_id, ftdmchan->chan_id, ftdmchan->physical_span_id, ftdmchan->physical_chan_id,
			ftdm_channel_state2str(ftdmchan->state), logmsg);
	ftdm_r2_write_log(level, file, function, line, completemsg);
}

static int ftdm_r2_on_dnis_digit_received(openr2_chan_t *r2chan, char digit)
{
	ftdm_sigmsg_t sigev;
	ftdm_r2_data_t *r2data;
	ftdm_channel_t *ftdmchan = openr2_chan_get_client_data(r2chan);
	ftdm_size_t collected_len = R2CALL(ftdmchan)->dnis_index;

	ftdm_log_chan(ftdmchan, FTDM_LOG_DEBUG, "DNIS digit %c received\n", digit);

	/* save the digit we just received */ 
	ftdmchan->caller_data.dnis.digits[collected_len] = digit;
	collected_len++;
	ftdmchan->caller_data.dnis.digits[collected_len] = '\0';
	R2CALL(ftdmchan)->dnis_index = collected_len;

	/* notify the user about the new digit and check if we should stop requesting more DNIS */
	memset(&sigev, 0, sizeof(sigev));
	sigev.chan_id = ftdmchan->chan_id;
	sigev.span_id = ftdmchan->span_id;
	sigev.channel = ftdmchan;
	sigev.event_id = FTDM_SIGEVENT_COLLECTED_DIGIT;
	r2data = ftdmchan->span->signal_data;
	if (ftdm_span_send_signal(ftdmchan->span, &sigev) == FTDM_BREAK) {
		ftdm_log_chan(ftdmchan, FTDM_LOG_NOTICE, "Requested to stop getting DNIS. Current DNIS = %s\n", ftdmchan->caller_data.dnis.digits);
		return OR2_STOP_DNIS_REQUEST; 
	}

	/* the only other reason to stop requesting DNIS is that there is no more room to save it */
	if (collected_len == (sizeof(ftdmchan->caller_data.dnis.digits) - 1)) {
		ftdm_log_chan(ftdmchan, FTDM_LOG_WARNING, "No more room for DNIS. Current DNIS = %s\n", ftdmchan->caller_data.dnis.digits);
		return OR2_STOP_DNIS_REQUEST;
	}

	return OR2_CONTINUE_DNIS_REQUEST; 
}

static void ftdm_r2_on_ani_digit_received(openr2_chan_t *r2chan, char digit)
{
	ftdm_channel_t *ftdmchan = openr2_chan_get_client_data(r2chan);
	ftdm_size_t collected_len = R2CALL(ftdmchan)->ani_index;

	/* check if we should drop ANI */
	if (collected_len == (sizeof(ftdmchan->caller_data.ani.digits) - 1)) {
		ftdm_log_chan(ftdmchan, FTDM_LOG_WARNING, "No more room for ANI, digit dropped: %c\n", digit);
		return;
	}
	ftdm_log_chan(ftdmchan, FTDM_LOG_DEBUG, "ANI digit %c received\n", digit);

	/* save the digit we just received */ 
	ftdmchan->caller_data.ani.digits[collected_len] = digit;
	collected_len++;
	ftdmchan->caller_data.ani.digits[collected_len] = '\0';
	R2CALL(ftdmchan)->ani_index = collected_len;
}

static openr2_event_interface_t ftdm_r2_event_iface = {
	/* .on_call_init */ ftdm_r2_on_call_init,
	/* .on_call_offered */ ftdm_r2_on_call_offered,
	/* .on_call_accepted */ ftdm_r2_on_call_accepted,
	/* .on_call_answered */ ftdm_r2_on_call_answered,
	/* .on_call_disconnect */ ftdm_r2_on_call_disconnect,
	/* .on_call_end */ ftdm_r2_on_call_end,
	/* .on_call_read */ ftdm_r2_on_call_read,
	/* .on_hardware_alarm */ ftdm_r2_on_hardware_alarm,
	/* .on_os_error */ ftdm_r2_on_os_error,
	/* .on_protocol_error */ ftdm_r2_on_protocol_error,
	/* .on_line_blocked */ ftdm_r2_on_line_blocked,
	/* .on_line_idle */ ftdm_r2_on_line_idle,

	/* cast seems to be needed to get rid of the annoying warning regarding format attribute  */
	/* .on_context_log */ (openr2_handle_context_logging_func)ftdm_r2_on_context_log,
	/* .on_dnis_digit_received */ ftdm_r2_on_dnis_digit_received,
	/* .on_ani_digit_received */ ftdm_r2_on_ani_digit_received,

	/* so far we do nothing with billing pulses */
	/* .on_billing_pulse_received */ NULL,
};

static int ftdm_r2_io_set_cas(openr2_chan_t *r2chan, int cas)
{
	ftdm_channel_t *ftdm_chan = openr2_chan_get_fd(r2chan);
	ftdm_status_t status = ftdm_channel_command(ftdm_chan, FTDM_COMMAND_SET_CAS_BITS, &cas);
	if (FTDM_FAIL == status) {
		return -1;
	}
	return 0;
}

static int ftdm_r2_io_get_cas(openr2_chan_t *r2chan, int *cas)
{
	ftdm_channel_t *ftdm_chan = openr2_chan_get_fd(r2chan);
	ftdm_status_t status = ftdm_channel_command(ftdm_chan, FTDM_COMMAND_GET_CAS_BITS, cas);
	if (FTDM_FAIL == status) {
		return -1;
	}
	return 0;
}

static int ftdm_r2_io_flush_write_buffers(openr2_chan_t *r2chan)
{
	ftdm_channel_t *ftdm_chan = openr2_chan_get_fd(r2chan);
	ftdm_status_t status = ftdm_channel_command(ftdm_chan, FTDM_COMMAND_FLUSH_TX_BUFFERS, NULL);
	if (FTDM_FAIL == status) {
		return -1;
	}
	return 0;
}

static int ftdm_r2_io_write(openr2_chan_t *r2chan, const void *buf, int size)
{
	ftdm_channel_t *ftdm_chan = openr2_chan_get_fd(r2chan);
	ftdm_size_t outsize = size;
	ftdm_status_t status = ftdm_channel_write(ftdm_chan, (void *)buf, size, &outsize);
	if (FTDM_FAIL == status) {
		return -1;
	}
	return outsize;
}

static int ftdm_r2_io_read(openr2_chan_t *r2chan, const void *buf, int size)
{
	ftdm_channel_t *ftdm_chan = openr2_chan_get_fd(r2chan);
	ftdm_size_t outsize = size;
	ftdm_status_t status = ftdm_channel_read(ftdm_chan, (void *)buf, &outsize);
	if (FTDM_FAIL == status) {
		return -1;
	}
	return outsize;
}

static int ftdm_r2_io_wait(openr2_chan_t *r2chan, int *flags, int block)
{
	ftdm_status_t status;
	int32_t timeout;
	ftdm_wait_flag_t ftdmflags = 0;

	ftdm_channel_t *fchan = openr2_chan_get_fd(r2chan);
	timeout = block ? -1 : 0;

	if (*flags & OR2_IO_READ) {
		ftdmflags |= FTDM_READ;
	}
	if (*flags & OR2_IO_WRITE) {
		ftdmflags |= FTDM_WRITE;
	}
	if (*flags & OR2_IO_OOB_EVENT) {
		ftdmflags |= FTDM_EVENTS;
	}
	
	status = ftdm_channel_wait(fchan, &ftdmflags, timeout);

	if (FTDM_SUCCESS != status && FTDM_TIMEOUT != status) {
		ftdm_log_chan_msg(fchan, FTDM_LOG_ERROR, "Failed to wait for events on channel\n");
		return -1;
	}

	*flags = 0;
	if (ftdmflags & FTDM_READ) {
		*flags |= OR2_IO_READ;
	}
	if (ftdmflags & FTDM_WRITE) {
		*flags |= OR2_IO_WRITE;
	}
	if (ftdmflags & FTDM_EVENTS) {
		*flags |= OR2_IO_OOB_EVENT;
	}

	return 0;
}

/* The following openr2 hooks never get called, read on for reasoning ... */
/* since freetdm takes care of opening the file descriptor and using openr2_chan_new_from_fd, openr2 should never call this hook */
static openr2_io_fd_t ftdm_r2_io_open(openr2_context_t *r2context, int channo)
{
	ftdm_log(FTDM_LOG_ERROR, "I should not be called (I/O open)!!\n");
	return NULL;
}

/* since freetdm takes care of closing the file descriptor and uses openr2_chan_new_from_fd, openr2 should never call this hook */
static int ftdm_r2_io_close(openr2_chan_t *r2chan)
{
	ftdm_channel_t *fchan = openr2_chan_get_client_data(r2chan);
	ftdm_log_chan_msg(fchan, FTDM_LOG_ERROR, "I should not be called (I/O close)!!\n");
	return 0;
}

/* since freetdm takes care of opening the file descriptor and using openr2_chan_new_from_fd, openr2 should never call this hook */
static int ftdm_r2_io_setup(openr2_chan_t *r2chan)
{
	ftdm_channel_t *fchan = openr2_chan_get_client_data(r2chan);
	ftdm_log_chan_msg(fchan, FTDM_LOG_ERROR, "I should not be called (I/O Setup)!!\n");
	return 0;
}

static int ftdm_r2_io_get_oob_event(openr2_chan_t *r2chan, openr2_oob_event_t *event)
{
    ftdm_status_t status;
    ftdm_event_t *fevent = NULL;
    ftdm_channel_t *ftdmchan = openr2_chan_get_fd(r2chan);

    *event = OR2_OOB_EVENT_NONE;
    status = ftdm_channel_read_event(ftdmchan, &fevent);
    if (status != FTDM_SUCCESS) {
		ftdm_log_chan_msg(ftdmchan, FTDM_LOG_ERROR, "failed to retrieve freetdm event!\n");
        return -1;
    }
	if (fevent->e_type != FTDM_EVENT_OOB)
		return 0;
	switch (fevent->enum_id) {
		case FTDM_OOB_CAS_BITS_CHANGE:
			{
				*event = OR2_OOB_EVENT_CAS_CHANGE;
			}
			break;
		case FTDM_OOB_ALARM_TRAP:
			{
				*event = OR2_OOB_EVENT_ALARM_ON;
			}
			break;
		case FTDM_OOB_ALARM_CLEAR:
			{
				*event = OR2_OOB_EVENT_ALARM_OFF;
			}
			break;
	}
    return 0;
}

static openr2_io_interface_t ftdm_r2_io_iface = {
	/* .open */ ftdm_r2_io_open, /* never called */
	/* .close */ ftdm_r2_io_close, /* never called */
	/* .set_cas */ ftdm_r2_io_set_cas,
	/* .get_cas */ ftdm_r2_io_get_cas,
	/* .flush_write_buffers */ ftdm_r2_io_flush_write_buffers,
	/* .write */ ftdm_r2_io_write,
	/* .read */ ftdm_r2_io_read,
	/* .setup */ ftdm_r2_io_setup, /* never called */
	/* .wait */ ftdm_r2_io_wait,
	/* .get_oob_event */ ftdm_r2_io_get_oob_event /* never called */
};

/* resolve a loglevel string, such as "debug,notice,warning",  to an openr2 log level integer */
static openr2_log_level_t ftdm_r2_loglevel_from_string(const char *level)
{
	openr2_log_level_t tmplevel;
	openr2_log_level_t newlevel = 0;
	char *clevel = NULL;
	char *logval = NULL;

	logval = ftdm_malloc(strlen(level)+1); /* alloca man page scared me, so better to use good ol' malloc  */
	if (!logval) {
		ftdm_log(FTDM_LOG_WARNING, "Ignoring R2 logging parameter: '%s', failed to alloc memory\n", level);
		return newlevel;
	}
	strcpy(logval, level);
	while (logval) {
		clevel = strsep(&logval, ",");
		if (-1 == (tmplevel = openr2_log_get_level(clevel))) {
			ftdm_log(FTDM_LOG_WARNING, "Ignoring invalid R2 logging level: '%s'\n", clevel);
			continue;
		}
		newlevel |= tmplevel;
	}
	ftdm_safe_free(logval);
	return newlevel;
}

static ftdm_state_map_t r2_state_map = {
	{
		{
			ZSD_INBOUND,
			ZSM_UNACCEPTABLE,
			{FTDM_CHANNEL_STATE_DOWN, FTDM_END},
			{FTDM_CHANNEL_STATE_COLLECT, FTDM_END}
		},
		{
			ZSD_INBOUND,
			ZSM_UNACCEPTABLE,
			{FTDM_CHANNEL_STATE_COLLECT, FTDM_END},
			{FTDM_CHANNEL_STATE_RING, FTDM_CHANNEL_STATE_TERMINATING, FTDM_END}
		},
		{
			ZSD_INBOUND,
			ZSM_UNACCEPTABLE,
			{FTDM_CHANNEL_STATE_RING, FTDM_END},
			{FTDM_CHANNEL_STATE_HANGUP, FTDM_CHANNEL_STATE_TERMINATING, FTDM_CHANNEL_STATE_PROGRESS, FTDM_CHANNEL_STATE_PROGRESS_MEDIA, FTDM_CHANNEL_STATE_UP, FTDM_END}
		},
		{
			ZSD_INBOUND,
			ZSM_UNACCEPTABLE,
			{FTDM_CHANNEL_STATE_HANGUP, FTDM_END},
			{FTDM_CHANNEL_STATE_DOWN, FTDM_END},
		},
		{
			ZSD_INBOUND,
			ZSM_UNACCEPTABLE,
			{FTDM_CHANNEL_STATE_TERMINATING, FTDM_END},
			{FTDM_CHANNEL_STATE_HANGUP, FTDM_END},
		},
		{
			ZSD_INBOUND,
			ZSM_UNACCEPTABLE,
			{FTDM_CHANNEL_STATE_PROGRESS, FTDM_END},
			{FTDM_CHANNEL_STATE_HANGUP, FTDM_CHANNEL_STATE_TERMINATING, FTDM_CHANNEL_STATE_PROGRESS_MEDIA, FTDM_CHANNEL_STATE_UP, FTDM_END},
		},
		{
			ZSD_INBOUND,
			ZSM_UNACCEPTABLE,
			{FTDM_CHANNEL_STATE_PROGRESS_MEDIA, FTDM_END},
			{FTDM_CHANNEL_STATE_HANGUP, FTDM_CHANNEL_STATE_TERMINATING, FTDM_CHANNEL_STATE_UP, FTDM_END},
		},
		{
			ZSD_INBOUND,
			ZSM_UNACCEPTABLE,
			{FTDM_CHANNEL_STATE_UP, FTDM_END},
			{FTDM_CHANNEL_STATE_HANGUP, FTDM_CHANNEL_STATE_TERMINATING, FTDM_END},
		},
		
		/* Outbound states */
		
		{
			ZSD_OUTBOUND,
			ZSM_UNACCEPTABLE,
			{FTDM_CHANNEL_STATE_DOWN, FTDM_END},
			{FTDM_CHANNEL_STATE_DIALING, FTDM_END}
		},

		{
			ZSD_OUTBOUND,
			ZSM_UNACCEPTABLE,
			{FTDM_CHANNEL_STATE_DIALING, FTDM_END},
			{FTDM_CHANNEL_STATE_HANGUP, FTDM_CHANNEL_STATE_TERMINATING, FTDM_CHANNEL_STATE_PROGRESS_MEDIA, FTDM_END}
		},

		{
			ZSD_OUTBOUND,
			ZSM_UNACCEPTABLE,
			{FTDM_CHANNEL_STATE_HANGUP, FTDM_END},
			{FTDM_CHANNEL_STATE_DOWN, FTDM_END}
		},

		{
			ZSD_OUTBOUND,
			ZSM_UNACCEPTABLE,
			{FTDM_CHANNEL_STATE_TERMINATING, FTDM_END},
			{FTDM_CHANNEL_STATE_HANGUP, FTDM_END}
		},

		{
			ZSD_OUTBOUND,
			ZSM_UNACCEPTABLE,
			{FTDM_CHANNEL_STATE_PROGRESS_MEDIA, FTDM_END},
			{FTDM_CHANNEL_STATE_HANGUP, FTDM_CHANNEL_STATE_TERMINATING, FTDM_CHANNEL_STATE_UP, FTDM_END}
		},

		{
			ZSD_OUTBOUND,
			ZSM_UNACCEPTABLE,
			{FTDM_CHANNEL_STATE_UP, FTDM_END},
			{FTDM_CHANNEL_STATE_HANGUP, FTDM_CHANNEL_STATE_TERMINATING, FTDM_END}
		},
	}
};

static FIO_CONFIGURE_SPAN_SIGNALING_FUNCTION(ftdm_r2_configure_span_signaling)
{
	unsigned int i = 0;
	int conf_failure = 0;
	const char *var = NULL, *val = NULL;
	const char *log_level = "notice,warning,error"; /* default loglevel, if none is read from conf */
	ftdm_r2_data_t *r2data = NULL;
	ftdm_r2_span_pvt_t *spanpvt = NULL;
	ftdm_r2_call_t *r2call = NULL;
	openr2_chan_t *r2chan = NULL;
	unsigned paramindex = 0;

	ft_r2_conf_t r2conf = 
	{
		/* .variant */ OR2_VAR_ITU,
		/* .category */ OR2_CALLING_PARTY_CATEGORY_NATIONAL_SUBSCRIBER,
		/* .loglevel */ OR2_LOG_ERROR | OR2_LOG_WARNING,
#ifdef WIN32
		/* .logdir */ (char *)"c:\\", 
#else
		/* .logdir */ (char *)"/tmp", 
#endif
		/* .advanced_protocol_file */ NULL,
		/* .max_ani */ 10,
		/* .max_dnis */ 4,
		/* .mfback_timeout */ -1,
		/* .metering_pulse_timeout */ -1,
		/* .mf_dump_size */ 0,
		/* .immediate_accept */ -1,
		/* .skip_category */ -1,
		/* .get_ani_first */ -1,
		/* .call_files */ 0,
		/* .double_answer */ -1,
		/* .charge_calls */ -1,
		/* .forced_release */ -1,
		/* .allow_collect_calls */ -1
	};

	ftdm_assert_return(sig_cb != NULL, FTDM_FAIL, "No signaling cb provided\n");

	if (span->signal_type) {
		snprintf(span->last_error, sizeof(span->last_error), "Span is already configured for signalling.");
		return FTDM_FAIL;
	}

	for (; ftdm_parameters[paramindex].var; paramindex++) {
		var = ftdm_parameters[paramindex].var;
		val = ftdm_parameters[paramindex].val;
		ftdm_log(FTDM_LOG_DEBUG, "Reading R2 parameter %s for span %d\n", var, span->span_id);
		if (!strcasecmp(var, "variant")) {
			if (!val) {
				break;
			}
			if (ftdm_strlen_zero_buf(val)) {
				ftdm_log(FTDM_LOG_NOTICE, "Ignoring empty R2 variant parameter\n");
				continue;
			}
			r2conf.variant = openr2_proto_get_variant(val);
			if (r2conf.variant == OR2_VAR_UNKNOWN) {
				ftdm_log(FTDM_LOG_ERROR, "Unknown R2 variant %s\n", val);
				conf_failure = 1;
				break;
			}
			ftdm_log(FTDM_LOG_DEBUG, "Configuring R2 span %d for variant %s\n", span->span_id, val);
		} else if (!strcasecmp(var, "category")) {
			if (!val) {
				break;
			}
			if (ftdm_strlen_zero_buf(val)) {
				ftdm_log(FTDM_LOG_NOTICE, "Ignoring empty R2 category parameter\n");
				continue;
			}
			r2conf.category = openr2_proto_get_category(val);
			if (r2conf.category == OR2_CALLING_PARTY_CATEGORY_UNKNOWN) {
				ftdm_log(FTDM_LOG_ERROR, "Unknown R2 caller category %s\n", val);
				conf_failure = 1;
				break;
			}
			ftdm_log(FTDM_LOG_DEBUG, "Configuring R2 span %d with default category %s\n", span->span_id, val);
		} else if (!strcasecmp(var, "logdir")) {
			if (!val) {
				break;
			}
			if (ftdm_strlen_zero_buf(val)) {
				ftdm_log(FTDM_LOG_NOTICE, "Ignoring empty R2 logdir parameter\n");
				continue;
			}
			r2conf.logdir = (char *)val;
			ftdm_log(FTDM_LOG_DEBUG, "Configuring R2 span %d with logdir %s\n", span->span_id, val);
		} else if (!strcasecmp(var, "logging")) {
			if (!val) {
				break;
			}
			if (ftdm_strlen_zero_buf(val)) {
				ftdm_log(FTDM_LOG_NOTICE, "Ignoring empty R2 logging parameter\n");
				continue;
			}
			log_level = val;
			ftdm_log(FTDM_LOG_DEBUG, "Configuring R2 span %s with loglevel %s\n", span->name, val);
		} else if (!strcasecmp(var, "advanced_protocol_file")) {
			if (!val) {
				break;
			}
			if (ftdm_strlen_zero_buf(val)) {
				ftdm_log(FTDM_LOG_NOTICE, "Ignoring empty R2 advanced_protocol_file parameter\n");
				continue;
			}
			r2conf.advanced_protocol_file = (char *)val;
			ftdm_log(FTDM_LOG_DEBUG, "Configuring R2 span %s with advanced protocol file %s\n", span->name, val);
		} else if (!strcasecmp(var, "mf_dump_size")) {
			r2conf.mf_dump_size = atoi(val);
			if (r2conf.mf_dump_size < 0) {
				r2conf.mf_dump_size = FTDM_IO_DUMP_DEFAULT_BUFF_SIZE;
				ftdm_log(FTDM_LOG_DEBUG, "Configuring R2 span %s with default mf_dump_size = %d bytes\n", span->name, r2conf.mf_dump_size);
			} else {
				ftdm_log(FTDM_LOG_DEBUG, "Configuring R2 span %s with mf_dump_size = %d bytes\n", span->name, r2conf.mf_dump_size);
			}
		} else if (!strcasecmp(var, "allow_collect_calls")) {
			r2conf.allow_collect_calls = ftdm_true(val);
			ftdm_log(FTDM_LOG_DEBUG, "Configuring R2 span %s with allow collect calls max ani = %d\n", span->name, r2conf.allow_collect_calls);
		} else if (!strcasecmp(var, "double_answer")) {
			r2conf.double_answer = ftdm_true(val);
			ftdm_log(FTDM_LOG_DEBUG, "Configuring R2 span %s with double answer = %d\n", span->name, r2conf.double_answer);
		} else if (!strcasecmp(var, "immediate_accept")) {
			r2conf.immediate_accept = ftdm_true(val);
			ftdm_log(FTDM_LOG_DEBUG, "Configuring R2 span %s with immediate accept = %d\n", span->name, r2conf.immediate_accept);
		} else if (!strcasecmp(var, "skip_category")) {
			r2conf.skip_category = ftdm_true(val);
			ftdm_log(FTDM_LOG_DEBUG, "Configuring R2 span %s with skip category = %d\n", span->name, r2conf.skip_category);
		} else if (!strcasecmp(var, "forced_release")) {
			r2conf.forced_release = ftdm_true(val);
			ftdm_log(FTDM_LOG_DEBUG, "Configuring R2 span %s with forced release = %d\n", span->name, r2conf.forced_release);
		} else if (!strcasecmp(var, "charge_calls")) {
			r2conf.charge_calls = ftdm_true(val);
			ftdm_log(FTDM_LOG_DEBUG, "Configuring R2 span %s with charge calls = %d\n", span->name, r2conf.charge_calls);
		} else if (!strcasecmp(var, "get_ani_first")) {
			r2conf.get_ani_first = ftdm_true(val);
			ftdm_log(FTDM_LOG_DEBUG, "Configuring R2 span %s with get ani first = %d\n", span->name, r2conf.get_ani_first);
		} else if (!strcasecmp(var, "call_files")) {
			r2conf.call_files = ftdm_true(val);
			ftdm_log(FTDM_LOG_DEBUG, "Configuring R2 span %s with call files = %d\n", span->name, r2conf.call_files);
		} else if (!strcasecmp(var, "mfback_timeout")) {
			r2conf.mfback_timeout = atoi(val);
			ftdm_log(FTDM_LOG_DEBUG, "Configuring R2 span %s with MF backward timeout = %dms\n", span->name, r2conf.mfback_timeout);
		} else if (!strcasecmp(var, "metering_pulse_timeout")) {
			r2conf.metering_pulse_timeout = atoi(val);
			ftdm_log(FTDM_LOG_DEBUG, "Configuring R2 span %s with metering pulse timeout = %dms\n", span->name, r2conf.metering_pulse_timeout);
		} else if (!strcasecmp(var, "max_ani")) {
			r2conf.max_ani = atoi(val);
			ftdm_log(FTDM_LOG_DEBUG, "Configuring R2 span %s with max ani = %d\n", span->name, r2conf.max_ani);
		} else if (!strcasecmp(var, "max_dnis")) {
			r2conf.max_dnis = atoi(val);
			ftdm_log(FTDM_LOG_DEBUG, "Configuring R2 span %s with max dnis = %d\n", span->name, r2conf.max_dnis);
		} else {
			snprintf(span->last_error, sizeof(span->last_error), "Unknown R2 parameter [%s]", var);
			return FTDM_FAIL;
		}
	}

	if (conf_failure) {
		snprintf(span->last_error, sizeof(span->last_error), "R2 configuration error");
		return FTDM_FAIL;
	}

	/* set span log level */
	r2conf.loglevel = ftdm_r2_loglevel_from_string(log_level);
	ftdm_log(FTDM_LOG_DEBUG, "Configuring R2 span %d with loglevel %s\n", span->span_id, log_level);

	r2data = ftdm_malloc(sizeof(*r2data));
	if (!r2data) {
		snprintf(span->last_error, sizeof(span->last_error), "Failed to allocate R2 data.");
		return FTDM_FAIL;
	}
	memset(r2data, 0, sizeof(*r2data));

	spanpvt = ftdm_malloc(sizeof(*spanpvt));
	if (!spanpvt) {
		snprintf(span->last_error, sizeof(span->last_error), "Failed to allocate private span data container.");
		goto fail;
	}
	memset(spanpvt, 0, sizeof(*spanpvt));

	r2data->r2context = openr2_context_new(r2conf.variant, &ftdm_r2_event_iface, r2conf.max_ani, r2conf.max_dnis);
	if (!r2data->r2context) {
		snprintf(span->last_error, sizeof(span->last_error), "Cannot create openr2 context for span.");
		goto fail;
	}
	openr2_context_set_io_type(r2data->r2context, OR2_IO_CUSTOM, &ftdm_r2_io_iface);
	openr2_context_set_log_level(r2data->r2context, r2conf.loglevel);
	openr2_context_set_ani_first(r2data->r2context, r2conf.get_ani_first);
	openr2_context_set_skip_category_request(r2data->r2context, r2conf.skip_category);
	openr2_context_set_mf_back_timeout(r2data->r2context, r2conf.mfback_timeout);
	openr2_context_set_metering_pulse_timeout(r2data->r2context, r2conf.metering_pulse_timeout);
	openr2_context_set_double_answer(r2data->r2context, r2conf.double_answer);
	openr2_context_set_immediate_accept(r2data->r2context, r2conf.immediate_accept);

	ftdm_log(FTDM_LOG_DEBUG, "Setting span %s logdir to %s\n", span->name, r2conf.logdir);
	openr2_context_set_log_directory(r2data->r2context, r2conf.logdir);
	snprintf(r2data->logdir, sizeof(r2data->logdir), "%s", r2conf.logdir);

	if (r2conf.advanced_protocol_file) {
		openr2_context_configure_from_advanced_file(r2data->r2context, r2conf.advanced_protocol_file);
	}

	spanpvt->r2calls = create_hashtable(FTDM_MAX_CHANNELS_SPAN, ftdm_hash_hashfromstring, ftdm_hash_equalkeys);
	if (!spanpvt->r2calls) {
		snprintf(span->last_error, sizeof(span->last_error), "Cannot create channel calls hash for span.");
		goto fail;
	}

	for (i = 1; (i <= span->chan_count) && (i <= FTDM_MAX_CHANNELS_SPAN); i++) {
		r2chan = openr2_chan_new_from_fd(r2data->r2context, span->channels[i], span->channels[i]->physical_chan_id);
		if (!r2chan) {
			snprintf(span->last_error, sizeof(span->last_error), "Cannot create all openr2 channels for span.");
			goto fail;
		}
		openr2_chan_set_log_level(r2chan, r2conf.loglevel);
		if (r2conf.call_files) {
			openr2_chan_enable_call_files(r2chan);
		}

		r2call = ftdm_malloc(sizeof(*r2call));
		if (!r2call) {
			snprintf(span->last_error, sizeof(span->last_error), "Cannot create all R2 call data structures for the span.");
			ftdm_safe_free(r2chan);
			goto fail;
		}
		memset(r2call, 0, sizeof(*r2call));
		openr2_chan_set_logging_func(r2chan, ftdm_r2_on_chan_log);
		openr2_chan_set_client_data(r2chan, span->channels[i]);
		r2call->r2chan = r2chan;
		span->channels[i]->call_data = r2call;
		/* value and key are the same so just free one of them */
		snprintf(r2call->name, sizeof(r2call->name), "chancall%d", i);
		hashtable_insert(spanpvt->r2calls, (void *)r2call->name, r2call, HASHTABLE_FLAG_FREE_VALUE);

	}
	r2data->mf_dump_size = r2conf.mf_dump_size;
	r2data->flags = 0;
	spanpvt->r2context = r2data->r2context;

	/* just the value must be freed by the hash */
	hashtable_insert(g_mod_data_hash, (void *)span->name, spanpvt, HASHTABLE_FLAG_FREE_VALUE);

	span->start = ftdm_r2_start;
	span->stop = ftdm_r2_stop;
	span->sig_read = NULL;

	/* let the core set the states, we just read them */
	span->get_channel_sig_status = ftdm_r2_get_channel_sig_status;

	span->signal_cb = sig_cb;
	span->signal_type = FTDM_SIGTYPE_R2;
	span->signal_data = r2data;
	span->outgoing_call = r2_outgoing_call;

	span->state_map = &r2_state_map;

	/* use signals queue */
	ftdm_set_flag(span, FTDM_SPAN_USE_SIGNALS_QUEUE);

	return FTDM_SUCCESS;

fail:

	if (r2data && r2data->r2context) {
		openr2_context_delete(r2data->r2context);
	}
	if (spanpvt && spanpvt->r2calls) {
		hashtable_destroy(spanpvt->r2calls);
	}
	ftdm_safe_free(r2data);
	ftdm_safe_free(spanpvt);
	return FTDM_FAIL;

}

/* the channel must be locked when calling this function */
static int ftdm_r2_state_advance(ftdm_channel_t *ftdmchan)
{
	ftdm_sigmsg_t sigev;
	int ret;
	openr2_chan_t *r2chan = R2CALL(ftdmchan)->r2chan;

	memset(&sigev, 0, sizeof(sigev));
	sigev.chan_id = ftdmchan->chan_id;
	sigev.span_id = ftdmchan->span_id;
	sigev.channel = ftdmchan;

	ret = 0;

	/* because we do not always acknowledge the state change (clearing the FTDM_CHANNEL_STATE_CHANGE flag) due to the accept
	 * procedure described below, we need the chanstate member to NOT process some states twice, so is valid entering this 
	 * function with the FTDM_CHANNEL_STATE_CHANGE flag set but with a state that was already processed and is just waiting
	 * to complete (the processing is media-bound)
	 * */
	if (ftdm_test_flag(ftdmchan, FTDM_CHANNEL_STATE_CHANGE) 
			&& (R2CALL(ftdmchan)->chanstate != ftdmchan->state)) {

		ftdm_log_chan(ftdmchan, FTDM_LOG_DEBUG, "Executing state handler for %s\n", ftdm_channel_state2str(ftdmchan->state));
		R2CALL(ftdmchan)->chanstate = ftdmchan->state;

		if (IS_ACCEPTING_PENDING(ftdmchan)) {
			/* 
			   Moving to PROGRESS, PROGRESS_MEDIA or UP means that we must accept the call first, and accepting
			   the call in R2 means sending a tone, then waiting for the acknowledge from the other end,
			   since all of that requires sending and detecting tones, it takes a few milliseconds (I'd say around 100)
			   which means during that time the user should not try to perform any operations like answer, hangup or anything
			   else, therefore we DO NOT clear the FTDM_CHANNEL_STATE_CHANGE flag here, we rely on ftdm_io.c to block
			   the user thread until we're done with the accept (see on_call_accepted callback) and then we clear the state change flag,
			   otherwise we have a race condition between freetdm calling openr2_chan_answer_call and openr2 accepting the call first, 
			   if freetdm calls openr2_chan_answer_call before the accept cycle completes, openr2 will fail to answer the call */
			ftdm_log_chan(ftdmchan, FTDM_LOG_DEBUG, "State ack for state %s will have to wait a bit\n", ftdm_channel_state2str(ftdmchan->state));
		} else if (ftdmchan->state != FTDM_CHANNEL_STATE_DOWN){
			ftdm_clear_flag(ftdmchan, FTDM_CHANNEL_STATE_CHANGE);
			ftdm_channel_complete_state(ftdmchan);
		}

		switch (ftdmchan->state) {

			/* starting an incoming call */
			case FTDM_CHANNEL_STATE_COLLECT: 
				{
					uint32_t interval = 0;
					ftdm_channel_command(ftdmchan, FTDM_COMMAND_GET_INTERVAL, &interval);
					ftdm_assert(interval != 0, "Invalid interval!");
					ftdm_log_chan(ftdmchan, FTDM_LOG_DEBUG, "Starting processing of incoming call with interval %d\n", interval);
					openr2_chan_enable_read(r2chan);
				}
				break;

				/* starting an outgoing call */
			case FTDM_CHANNEL_STATE_DIALING:
				{
					uint32_t interval = 0;
					ftdm_channel_command(ftdmchan, FTDM_COMMAND_GET_INTERVAL, &interval);
					ftdm_assert(interval != 0, "Invalid interval!");
					ftdm_log_chan(ftdmchan, 
						FTDM_LOG_DEBUG, "Starting processing of outgoing call in channel with interval %d\n", interval);
					openr2_chan_enable_read(r2chan);
				}
				break;

				/* incoming call was offered */
			case FTDM_CHANNEL_STATE_RING:

				/* notify the user about the new call */
				sigev.event_id = FTDM_SIGEVENT_START;

				if (ftdm_span_send_signal(ftdmchan->span, &sigev) != FTDM_SUCCESS) {
					ftdm_log_chan_msg(ftdmchan, FTDM_LOG_NOTICE, "Failed to handle call offered\n");
					openr2_chan_disconnect_call(r2chan, OR2_CAUSE_OUT_OF_ORDER);
					ftdm_set_state(ftdmchan, FTDM_CHANNEL_STATE_CANCEL);
					break; 
				}
				R2CALL(ftdmchan)->ftdm_call_started = 1; 

				break;

				/* the call is making progress */
			case FTDM_CHANNEL_STATE_PROGRESS:
			case FTDM_CHANNEL_STATE_PROGRESS_MEDIA:
				{
					if (!ftdm_test_flag(ftdmchan, FTDM_CHANNEL_OUTBOUND)) {
						if (!R2CALL(ftdmchan)->accepted) {
							ftdm_log_chan_msg(ftdmchan, FTDM_LOG_DEBUG, "Accepting call\n");
							ft_r2_accept_call(ftdmchan);
						} 
					} else {
						ftdm_log_chan_msg(ftdmchan, FTDM_LOG_DEBUG, "Notifying progress\n");
						sigev.event_id = FTDM_SIGEVENT_PROCEED;
						ftdm_span_send_signal(ftdmchan->span, &sigev);

						sigev.event_id = FTDM_SIGEVENT_PROGRESS_MEDIA;
						ftdm_span_send_signal(ftdmchan->span, &sigev);
					}
				}
				break;

				/* the call was answered */
			case FTDM_CHANNEL_STATE_UP:
				{
					ftdm_log_chan_msg(ftdmchan, FTDM_LOG_DEBUG, "Call was answered\n");
					if (!ftdm_test_flag(ftdmchan, FTDM_CHANNEL_OUTBOUND)) {
						if (!R2CALL(ftdmchan)->accepted) {
							ftdm_log_chan_msg(ftdmchan, FTDM_LOG_DEBUG, "Call has not been accepted, need to accept first\n");
							// the answering will be done in the on_call_accepted handler
							ft_r2_accept_call(ftdmchan);
							R2CALL(ftdmchan)->answer_pending = 1;
						} else {
							ft_r2_answer_call(ftdmchan);
						}
					} else {
						ftdm_log_chan_msg(ftdmchan, FTDM_LOG_DEBUG, "Notifying of call answered\n");
						sigev.event_id = FTDM_SIGEVENT_UP;
						ftdm_span_send_signal(ftdmchan->span, &sigev);
					}
				}
				break;

				/* just got hangup */
			case FTDM_CHANNEL_STATE_HANGUP:
				{
					if (!R2CALL(ftdmchan)->disconnect_rcvd) {
						openr2_call_disconnect_cause_t disconnect_cause = ftdm_r2_ftdm_cause_to_openr2_cause(ftdmchan);
						ftdm_log_chan(ftdmchan, FTDM_LOG_DEBUG, "Clearing call, cause = %s\n", openr2_proto_get_disconnect_string(disconnect_cause));
						/* this will disconnect the call, but need to wait for the call end before moving to DOWN */
						openr2_chan_disconnect_call(r2chan, disconnect_cause);
					} else if (!R2CALL(ftdmchan)->protocol_error) {
						/* just ack the hangup, on_call_end will be called by openr2 right after */
						openr2_chan_disconnect_call(r2chan, OR2_CAUSE_NORMAL_CLEARING);
					} else {
						ftdm_log_chan_msg(ftdmchan, FTDM_LOG_ERROR, "Clearing call due to protocol error\n");
						ftdm_set_state(ftdmchan, FTDM_CHANNEL_STATE_DOWN);
					}
				}
				break;

			case FTDM_CHANNEL_STATE_TERMINATING:
				{
					/* if the call has not been started yet we must go to HANGUP right here */ 
					if (!R2CALL(ftdmchan)->ftdm_call_started) {
						ftdm_set_state(ftdmchan, FTDM_CHANNEL_STATE_HANGUP);
					} else {
						openr2_call_disconnect_cause_t disconnect_cause = ftdm_r2_ftdm_cause_to_openr2_cause(ftdmchan);
						ftdm_log_chan(ftdmchan, FTDM_LOG_DEBUG, "Clearing call, cause = %s\n", openr2_proto_get_disconnect_string(disconnect_cause));
						/* notify the user of the call terminating and we wait for the user to move us to hangup */
						sigev.event_id = FTDM_SIGEVENT_STOP;
						ftdm_span_send_signal(ftdmchan->span, &sigev);
					}
				}
				break;

				/* just got hangup from the freetdm side due to abnormal failure */
			case FTDM_CHANNEL_STATE_CANCEL:
				{
					ftdm_log_chan_msg(ftdmchan, FTDM_LOG_DEBUG, "Unable to receive call\n");
					openr2_chan_disconnect_call(r2chan, OR2_CAUSE_OUT_OF_ORDER);
				}
				break;

				/* finished call for good */
			case FTDM_CHANNEL_STATE_DOWN: 
				{
					ftdm_log_chan_msg(ftdmchan, FTDM_LOG_DEBUG, "Call is down\n");
					if (R2CALL(ftdmchan)->txdrops) {
						ftdm_log_chan(ftdmchan, FTDM_LOG_WARNING, "dropped %d tx packets\n", R2CALL(ftdmchan)->txdrops);
					}
					openr2_chan_disable_read(r2chan);
					ret = 1;
				}
				break;

			default:
				{
					ftdm_log_chan(ftdmchan, FTDM_LOG_ERROR, "Unhandled channel state change: %s\n", ftdm_channel_state2str(ftdmchan->state));
				}
				break;

		}
	}

	if (ret) {
		ftdm_channel_t *closed_chan;
		closed_chan = ftdmchan;
		ftdm_channel_close(&closed_chan);
		ftdm_log_chan_msg(ftdmchan, FTDM_LOG_DEBUG, "State processing ended.\n");
	}
	return ret;
}

/* the channel must be locked when calling this function */
static void ftdm_r2_state_advance_all(ftdm_channel_t *ftdmchan)
{
	/* because we do not always acknowledge the state change (clearing the FTDM_CHANNEL_STATE_CHANGE flag) due to the accept
	 * procedure described below, we need the chanstate member to NOT process some states twice, so is valid entering this 
	 * function with the FTDM_CHANNEL_STATE_CHANGE flag set but with a state that was already processed and is just waiting
	 * to complete (the processing is media-bound)
	 * */
	while (ftdm_test_flag(ftdmchan, FTDM_CHANNEL_STATE_CHANGE)
		&& (R2CALL(ftdmchan)->chanstate != ftdmchan->state)) {
		ftdm_r2_state_advance(ftdmchan);
	}
}

static void *ftdm_r2_run(ftdm_thread_t *me, void *obj)
{
	openr2_chan_t *r2chan;
	ftdm_channel_t *ftdmchan = NULL;
	ftdm_status_t status;
	ftdm_span_t *span = (ftdm_span_t *) obj;
	ftdm_r2_data_t *r2data = span->signal_data;
	int waitms = 20;
	unsigned int i;
	int res, ms;
	int index = 0;
	struct timeval start, end;
	ftdm_iterator_t *chaniter = NULL;
	short *poll_events = ftdm_malloc(sizeof(short) * span->chan_count);

#ifdef __linux__
	r2data->monitor_thread_id = syscall(SYS_gettid);	
#endif
	
	ftdm_log(FTDM_LOG_DEBUG, "OpenR2 monitor thread %lu started.\n", r2data->monitor_thread_id);
	r2chan = NULL;
	for (i = 1; i <= span->chan_count; i++) {
		r2chan = R2CALL(span->channels[i])->r2chan;
		openr2_chan_set_idle(r2chan);
		openr2_chan_process_cas_signaling(r2chan);

		/* set r2chan span id */
		openr2_chan_set_span_id(r2chan, span->span_id);

		ftdmchan = openr2_chan_get_client_data(r2chan);
		//ftdm_channel_set_feature(ftdmchan, FTDM_CHANNEL_FEATURE_IO_STATS);
	}

	memset(&start, 0, sizeof(start));
	memset(&end, 0, sizeof(end));
	chaniter = ftdm_span_get_chan_iterator(span, NULL);
	while (ftdm_running() && ftdm_test_flag(r2data, FTDM_R2_RUNNING)) {
		res = gettimeofday(&end, NULL);
		if (start.tv_sec) {
			ms = ((end.tv_sec - start.tv_sec) * 1000) 
			    + ((( 1000000 + end.tv_usec - start.tv_usec) / 1000) - 1000);
			if (ms < 0) {
				ms = 0;
			}
			if (ms > r2data->jobmax) {
				r2data->jobmax = ms;
			}
			index = (ms / 10);
			index = (index > 10) ? 10 : index;
			r2data->loops[index]++;
			r2data->total_loops++;
		}

#ifndef WIN32
		 /* figure out what event to poll each channel for. POLLPRI when the channel is down,
		  * POLLPRI|POLLIN|POLLOUT otherwise */
		memset(poll_events, 0, sizeof(short)*span->chan_count);
		chaniter = ftdm_span_get_chan_iterator(span, chaniter);
		for (i = 0; chaniter; chaniter = ftdm_iterator_next(chaniter), i++) {
			ftdmchan = ftdm_iterator_current(chaniter);
			r2chan = R2CALL(ftdmchan)->r2chan;
			poll_events[i] = POLLPRI;
			if (openr2_chan_get_read_enabled(r2chan)) {
				poll_events[i] |= POLLIN;
			}
		}

		status = ftdm_span_poll_event(span, waitms, poll_events);
#else
		status = ftdm_span_poll_event(span, waitms, NULL);
#endif

		res = gettimeofday(&start, NULL);
		if (res) {
			ftdm_log(FTDM_LOG_CRIT, "Failure gettimeofday [%s]\n", strerror(errno));
		}

		if (FTDM_FAIL == status) {
			ftdm_log(FTDM_LOG_CRIT, "Failure waiting I/O! [%s]\n", span->channels[1]->last_error);
			continue;
		}

		/* this main loop takes care of MF and CAS signaling during call setup and tear down
		 * for every single channel in the span, do not perform blocking operations here! */
		chaniter = ftdm_span_get_chan_iterator(span, chaniter);
		for ( ; chaniter; chaniter = ftdm_iterator_next(chaniter)) {
			ftdmchan = ftdm_iterator_current(chaniter);

			ftdm_mutex_lock(ftdmchan->mutex);

			ftdm_r2_state_advance_all(ftdmchan);

			r2chan = R2CALL(ftdmchan)->r2chan;
			openr2_chan_process_signaling(r2chan);

			ftdm_r2_state_advance_all(ftdmchan);

			ftdm_mutex_unlock(ftdmchan->mutex);
		}
		/* deliver the actual events to the user now without any channel locking */
		ftdm_span_trigger_signals(span);
	}
	
	chaniter = ftdm_span_get_chan_iterator(span, chaniter);
	for ( ; chaniter; chaniter = ftdm_iterator_next(chaniter)) {
		ftdmchan = ftdm_iterator_current(chaniter);
		r2chan = R2CALL(ftdmchan)->r2chan;
		openr2_chan_set_blocked(r2chan);
	}

	ftdm_iterator_free(chaniter);
	ftdm_safe_free(poll_events);

	ftdm_clear_flag(r2data, FTDM_R2_RUNNING);
	ftdm_log(FTDM_LOG_DEBUG, "R2 thread ending.\n");

	return NULL;
}

static void __inline__ block_channel(ftdm_channel_t *fchan, ftdm_stream_handle_t *stream)
{
	openr2_chan_t *r2chan = R2CALL(fchan)->r2chan;
	ftdm_mutex_lock(fchan->mutex);
	if (fchan->state != FTDM_CHANNEL_STATE_DOWN) {
		stream->write_function(stream, "cannot block channel %d:%d because has a call in progress\n", 
				fchan->span_id, fchan->chan_id);
	} else if (ftdm_test_flag(fchan, FTDM_CHANNEL_SUSPENDED)) {
		stream->write_function(stream, "cannot block channel %d:%d because is already blocked\n", 
				fchan->span_id, fchan->chan_id);
	} else {
		if (!openr2_chan_set_blocked(r2chan)) {
			ftdm_set_flag(fchan, FTDM_CHANNEL_SUSPENDED);
			stream->write_function(stream, "blocked channel %d:%d\n", 
					fchan->span_id, fchan->chan_id);
		} else {
			stream->write_function(stream, "failed to block channel %d:%d\n", 
					fchan->span_id, fchan->chan_id);
		}
	}
	ftdm_mutex_unlock(fchan->mutex);
}

static void __inline__ unblock_channel(ftdm_channel_t *fchan, ftdm_stream_handle_t *stream)
{
	openr2_chan_t *r2chan = R2CALL(fchan)->r2chan;
	ftdm_mutex_lock(fchan->mutex);
	if (ftdm_test_flag(fchan, FTDM_CHANNEL_SUSPENDED)) {
		if (!openr2_chan_set_idle(r2chan)) {
			ftdm_clear_flag(fchan, FTDM_CHANNEL_SUSPENDED);
			stream->write_function(stream, "unblocked channel %d:%d\n", 
					fchan->span_id, fchan->chan_id);
		} else {
			stream->write_function(stream, "failed to unblock channel %d:%d\n", 
					fchan->span_id, fchan->chan_id);
		}
	} else {
		stream->write_function(stream, "cannot unblock channel %d:%d because is not blocked\n", 
				fchan->span_id, fchan->chan_id);
	}
	ftdm_mutex_unlock(fchan->mutex);
}

static FIO_API_FUNCTION(ftdm_r2_api)
{
	ftdm_span_t *span = NULL;
	char *mycmd = NULL, *argv[10] = { 0 };
	int argc = 0;
	int span_id = 0;
	unsigned int chan_id = 0;
	unsigned int i = 0;
	ftdm_r2_data_t *r2data = NULL;
	openr2_chan_t *r2chan = NULL;
	openr2_context_t *r2context = NULL;
	openr2_variant_t r2variant;

	if (data) {
		mycmd = ftdm_strdup(data);
		argc = ftdm_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
	}

	if (argc >= 2) {
		if (!strcasecmp(argv[0], "block")) {
			int span_id = atoi(argv[1]);

			if (ftdm_span_find_by_name(argv[1], &span) == FTDM_SUCCESS || ftdm_span_find(span_id, &span) == FTDM_SUCCESS) {

				if (span->start != ftdm_r2_start) {
					stream->write_function(stream, "-ERR invalid span.\n");
					goto done;
				}

				if (argc > 2) {
					chan_id = atoi(argv[2]);
					if (chan_id && chan_id <= span->chan_count) {
						block_channel(span->channels[chan_id], stream);
					} else {
						stream->write_function(stream, "-ERR invalid chan %d.\n", chan_id);
					}
				} else {
					for (i = 1; i <= span->chan_count; i++) {
						block_channel(span->channels[i], stream);
					}
				}
				stream->write_function(stream, "+OK blocked.\n");
				goto done;
			} else {
				stream->write_function(stream, "-ERR invalid span.\n");
				goto done;
			}
		}

		if (!strcasecmp(argv[0], "unblock")) {
			span_id = atoi(argv[1]);
			if (ftdm_span_find_by_name(argv[1], &span) == FTDM_SUCCESS || ftdm_span_find(span_id, &span) == FTDM_SUCCESS) {

				if (span->start != ftdm_r2_start) {
					stream->write_function(stream, "-ERR invalid span.\n");
					goto done;
				}

				if (argc > 2) {
					chan_id = atoi(argv[2]);
					if (chan_id && chan_id <= span->chan_count) {
						unblock_channel(span->channels[chan_id], stream);
					} else {
						stream->write_function(stream, "-ERR invalid chan %d.\n", chan_id);
					}
				} else {
					for (i = 1; i <= span->chan_count; i++) {
						unblock_channel(span->channels[i], stream);
					}
				}
				
				stream->write_function(stream, "+OK.\n");
				goto done;
			} else {
				stream->write_function(stream, "-ERR invalid span.\n");
				goto done;
			}

		}

		if (!strcasecmp(argv[0], "status")) {
			//openr2_chan_stats_t stats;
			span_id = atoi(argv[1]);

			if (ftdm_span_find_by_name(argv[1], &span) == FTDM_SUCCESS || ftdm_span_find(span_id, &span) == FTDM_SUCCESS) {
				if (span->start != ftdm_r2_start) {
					stream->write_function(stream, "-ERR not an R2 span.\n");
					goto done;
				}
				if (!(r2data =  span->signal_data)) {
					stream->write_function(stream, "-ERR invalid span. No R2 signal data in span.\n");
					goto done;
				}
				r2context = r2data->r2context;
				r2variant = openr2_context_get_variant(r2context);
				stream->write_function(stream, 
						"Variant: %s\n"
						"Max ANI: %d\n"
						"Max DNIS: %d\n"
						"ANI First: %s\n"
						"Immediate Accept: %s\n"
						"Job Thread: %lu\n"
						"Job Max ms: %d\n"
						"Job Loops: %lu\n",
						openr2_proto_get_variant_string(r2variant),
						openr2_context_get_max_ani(r2context),
						openr2_context_get_max_dnis(r2context),
						openr2_context_get_ani_first(r2context) ? "Yes" : "No",
						openr2_context_get_immediate_accept(r2context) ? "Yes" : "No",
						r2data->monitor_thread_id,
						r2data->jobmax, 
						r2data->total_loops);
				stream->write_function(stream, "\n");
				stream->write_function(stream, "%4s %-12.12s %-12.12s\n", "Channel", "Tx CAS", "Rx CAS");
				for (i = 1; i <= span->chan_count; i++) {
					r2chan = R2CALL(span->channels[i])->r2chan;
					stream->write_function(stream, "%4d    %-12.12s %-12.12s\n", 
							span->channels[i]->chan_id,
							openr2_chan_get_tx_cas_string(r2chan),
							openr2_chan_get_rx_cas_string(r2chan));
				}
				stream->write_function(stream, "\n");
				stream->write_function(stream, "+OK.\n");
				goto done;
			} else {
				stream->write_function(stream, "-ERR invalid span.\n");
				goto done;
			}
		}

		if (!strcasecmp(argv[0], "loopstats")) {
			int range;
			float pct;
			span_id = atoi(argv[1]);

			if (ftdm_span_find_by_name(argv[1], &span) == FTDM_SUCCESS || ftdm_span_find(span_id, &span) == FTDM_SUCCESS) {
				if (span->start != ftdm_r2_start) {
					stream->write_function(stream, "-ERR not an R2 span.\n");
					goto done;
				}
				if (!(r2data =  span->signal_data)) {
					stream->write_function(stream, "-ERR invalid span. No R2 signal data in span.\n");
					goto done;
				}
				range = 0;
				for (i = 0; i < ftdm_array_len(r2data->loops); i++) {
					pct = 100*(float)r2data->loops[i]/r2data->total_loops;
					if ((i + 1) == ftdm_array_len(r2data->loops)) {
						stream->write_function(stream, ">= %dms: %llu - %.03lf%%\n", range, r2data->loops[i], pct);
					} else {
						stream->write_function(stream, "%d-%dms: %llu - %.03lf%%\n", range, range + 9, r2data->loops[i], pct);
					}
					range += 10;
				}
				stream->write_function(stream, "\n");
				stream->write_function(stream, "+OK.\n");
				goto done;
			} else {
				stream->write_function(stream, "-ERR invalid span.\n");
				goto done;
			}
		}

	}

	if (argc == 1) {
		if (!strcasecmp(argv[0], "threads")) {
			ftdm_mutex_lock(g_thread_count_mutex);
			stream->write_function(stream, "%d R2 channel threads up\n", g_thread_count);
			ftdm_mutex_unlock(g_thread_count_mutex);
			stream->write_function(stream, "+OK.\n");
			goto done;
		}

		if (!strcasecmp(argv[0], "version")) {
			stream->write_function(stream, "OpenR2 version: %s, revision: %s\n", openr2_get_version(), openr2_get_revision());
			stream->write_function(stream, "+OK.\n");
			goto done;
		}

		if (!strcasecmp(argv[0], "variants")) {
			int32_t numvariants = 0;
			const openr2_variant_entry_t *variants = openr2_proto_get_variant_list(&numvariants);
			if (!variants) {
				stream->write_function(stream, "-ERR failed to retrieve openr2 variant list.\n");
				goto done;
			}
#define VARIANT_FORMAT "%4s %40s\n"
			stream->write_function(stream, VARIANT_FORMAT, "Variant Code", "Country");
			numvariants--;
			for (; numvariants; numvariants--) {
				stream->write_function(stream, VARIANT_FORMAT, variants[numvariants].name, variants[numvariants].country);
			}
			stream->write_function(stream, "+OK.\n");
#undef VARIANT_FORMAT
			goto done;
		}
	}

	stream->write_function(stream, "-ERR invalid command.\n");

done:

	ftdm_safe_free(mycmd);

	return FTDM_SUCCESS;

}

static FIO_IO_LOAD_FUNCTION(ftdm_r2_io_init)
{
	assert(fio != NULL);
	memset(&g_ftdm_r2_interface, 0, sizeof(g_ftdm_r2_interface));

	g_ftdm_r2_interface.name = "r2";
	g_ftdm_r2_interface.api = ftdm_r2_api;

	*fio = &g_ftdm_r2_interface;

	return FTDM_SUCCESS;
}

static FIO_SIG_LOAD_FUNCTION(ftdm_r2_init)
{
	g_mod_data_hash = create_hashtable(10, ftdm_hash_hashfromstring, ftdm_hash_equalkeys);
	if (!g_mod_data_hash) {
		return FTDM_FAIL;
	}
	ftdm_mutex_create(&g_thread_count_mutex);
	return FTDM_SUCCESS;
}

static FIO_SIG_UNLOAD_FUNCTION(ftdm_r2_destroy)
{
	ftdm_hash_iterator_t *i = NULL;
	ftdm_r2_span_pvt_t *spanpvt = NULL;
	const void *key = NULL;
	void *val = NULL;
	for (i = hashtable_first(g_mod_data_hash); i; i = hashtable_next(i)) {
		hashtable_this(i, &key, NULL, &val);
		if (key && val) {
			spanpvt = val;
			openr2_context_delete(spanpvt->r2context);
			hashtable_destroy(spanpvt->r2calls);
		}
	}
	hashtable_destroy(g_mod_data_hash);
	ftdm_mutex_destroy(&g_thread_count_mutex);
	return FTDM_SUCCESS;
}

EX_DECLARE_DATA ftdm_module_t ftdm_module = { 
	/* .name */ "r2",
	/* .io_load */ ftdm_r2_io_init,
	/* .io_unload */ NULL,
	/* .sig_load */ ftdm_r2_init,
	/* .sig_configure */ NULL,
	/* .sig_unload */ ftdm_r2_destroy,
	/* .configure_span_signaling */ ftdm_r2_configure_span_signaling
};
	

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4
 */
