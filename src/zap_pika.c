/*
 * Copyright (c) 2007, Anthony Minessale II
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


#include "openzap.h"
#include "zap_pika.h"
#define MAX_NUMBER_OF_TRUNKS 64
#define PIKA_BLOCK_SIZE 160
#define PIKA_BLOCK_LEN 20

struct pika_channel_profile {
	char name[80];
	PKH_TRecordConfig record_config;
	PKH_TPlayConfig play_config;
	int ec_enabled;
	PKH_TECConfig ec_config;
};
typedef struct pika_channel_profile pika_channel_profile_t;

static struct {
	PKH_TSystemDeviceList board_list;
	TPikaHandle open_boards[MAX_NUMBER_OF_TRUNKS];
	TPikaHandle system_handle;
	PKH_TSystemConfig system_config;
	zap_hash_t *profile_hash;
} globals;


struct pika_span_data {
	TPikaHandle event_queue;
	PKH_TPikaEvent last_oob_event;
	uint32_t boardno;
};
typedef struct pika_span_data pika_span_data_t;

struct pika_chan_data {
	TPikaHandle handle;
	TPikaHandle media_in;
	TPikaHandle media_out;
	TPikaHandle media_in_queue;
	TPikaHandle media_out_queue;
	PKH_TPikaEvent last_media_event;
	PKH_TPikaEvent last_oob_event;
	PKH_TRecordConfig record_config;
	PKH_TPlayConfig play_config;
	int ec_enabled;
	PKH_TECConfig ec_config;
	zap_buffer_t *digit_buffer;
	zap_mutex_t *digit_mutex;
	zap_size_t dtmf_len;
};
typedef struct pika_chan_data pika_chan_data_t;

static char *pika_board_type_string(PK_UINT type)
{
	if (type == PKH_BOARD_TYPE_DIGITAL_GATEWAY) {
		return "digital_gateway";
	}

	if (type == PKH_BOARD_TYPE_ANALOG_GATEWAY) {
		return "analog_gateway";
	}

	return "unknown";
}

static ZIO_CONFIGURE_FUNCTION(pika_configure)
{
	pika_channel_profile_t *profile = NULL;
	int ok = 1;

	if (!(profile = (pika_channel_profile_t *) hashtable_search(globals.profile_hash, (char *)category))) {
		profile = malloc(sizeof(*profile));
		memset(profile, 0, sizeof(*profile));
		zap_set_string(profile->name, category);
		hashtable_insert(globals.profile_hash, (void *)profile->name, profile);
		zap_log(ZAP_LOG_INFO, "creating profile [%s]\n", category);
	}

	if (!strcasecmp(var, "rx-gain")) {
		profile->record_config.gain = (PK_FLOAT)atof(val);
	} else if (!strcasecmp(var, "rx-agc-enabled")) {
		profile->record_config.AGC.enabled = zap_true(val);
	} else if (!strcasecmp(var, "rx-agc-targetPower")) {
		profile->record_config.AGC.targetPower = (PK_FLOAT)atof(val);
	} else if (!strcasecmp(var, "rx-agc-minGain")) {
		profile->record_config.AGC.minGain = (PK_FLOAT)atof(val);
	} else if (!strcasecmp(var, "rx-agc-maxGain")) {
		profile->record_config.AGC.maxGain = (PK_FLOAT)atof(val);
	} else if (!strcasecmp(var, "rx-agc-attackRate")) {
		profile->record_config.AGC.attackRate = atoi(val);
	} else if (!strcasecmp(var, "rx-agc-decayRate")) {
		profile->record_config.AGC.decayRate = atoi(val);
	} else if (!strcasecmp(var, "rx-agc-speechThreshold")) {
		profile->record_config.AGC.speechThreshold = (PK_FLOAT)atof(val);
	} else if (!strcasecmp(var, "rx-vad-enabled")) {
		profile->record_config.VAD.enabled = zap_true(val);
	} else if (!strcasecmp(var, "rx-vad-activationThreshold")) {
		profile->record_config.VAD.activationThreshold = (PK_FLOAT)atof(val);
	} else if (!strcasecmp(var, "rx-vad-activationDebounceTime")) {
		profile->record_config.VAD.activationDebounceTime = atoi(val);
	} else if (!strcasecmp(var, "rx-vad-deactivationThreshold")) {
		profile->record_config.VAD.deactivationThreshold = (PK_FLOAT)atof(val);
	} else if (!strcasecmp(var, "rx-vad-deactivationDebounceTime")) {
		profile->record_config.VAD.deactivationDebounceTime = atoi(val);
	} else if (!strcasecmp(var, "rx-vad-preSpeechBufferSize")) {
		profile->record_config.VAD.preSpeechBufferSize = atoi(val);
	} else if (!strcasecmp(var, "tx-gain")) {
		profile->play_config.gain = (PK_FLOAT)atof(val);
	} else if (!strcasecmp(var, "tx-agc-enabled")) {
		profile->play_config.AGC.enabled = zap_true(val);
	} else if (!strcasecmp(var, "tx-agc-targetPower")) {
		profile->play_config.AGC.targetPower = (PK_FLOAT)atof(val);
	} else if (!strcasecmp(var, "tx-agc-minGain")) {
		profile->play_config.AGC.minGain = (PK_FLOAT)atof(val);
	} else if (!strcasecmp(var, "tx-agc-maxGain")) {
		profile->play_config.AGC.maxGain = (PK_FLOAT)atof(val);
	} else if (!strcasecmp(var, "tx-agc-attackRate")) {
		profile->play_config.AGC.attackRate = atoi(val);
	} else if (!strcasecmp(var, "tx-agc-decayRate")) {
		profile->play_config.AGC.decayRate = atoi(val);
	} else if (!strcasecmp(var, "tx-agc-speechThreshold")) {
		profile->play_config.AGC.speechThreshold = (PK_FLOAT)atof(val);
	} else if (!strcasecmp(var, "ec-enabled")) {
		profile->ec_enabled = zap_true(val);
	} else if (!strcasecmp(var, "ec-doubleTalkerThreshold")) {
		profile->ec_config.doubleTalkerThreshold = (PK_FLOAT)atof(val);
	} else if (!strcasecmp(var, "ec-speechPresentThreshold")) {
		profile->ec_config.speechPresentThreshold = (PK_FLOAT)atof(val);
	} else if (!strcasecmp(var, "ec-echoSuppressionThreshold")) {
		profile->ec_config.echoSuppressionThreshold = (PK_FLOAT)atof(val);
	} else if (!strcasecmp(var, "ec-echoSuppressionEnabled")) {
		profile->ec_config.echoSuppressionEnabled = zap_true(val);
	} else if (!strcasecmp(var, "ec-comfortNoiseEnabled")) {
		profile->ec_config.comfortNoiseEnabled = zap_true(val);
	} else if (!strcasecmp(var, "ec-adaptationModeEnabled")) {
		profile->ec_config.adaptationModeEnabled = zap_true(val);
	} else {
		ok = 0;
	}

	if (ok) {
		zap_log(ZAP_LOG_INFO, "setting param [%s]=[%s] for profile [%s]\n", var, val, category);
	} else {
		zap_log(ZAP_LOG_ERROR, "unknown param [%s]\n", var);
	}

	return ZAP_SUCCESS;
}

PK_VOID PK_CALLBACK media_out_callback(PKH_TPikaEvent *event)
{
	PK_STATUS pk_status;
	zap_channel_t *zchan = event->userData;
	pika_chan_data_t *chan_data = (pika_chan_data_t *) zchan->mod_data;

	//PK_CHAR g_EventText[PKH_EVENT_MAX_NAME_LENGTH];
	//PKH_EVENT_GetText(event->id, g_EventText, sizeof(g_EventText));
	//zap_log(ZAP_LOG_DEBUG, "Event: %s\n", g_EventText);
	
	switch (event->id) {
	case PKH_EVENT_PLAY_IDLE:
		{
			while (zap_buffer_inuse(chan_data->digit_buffer)) {
				char dtmf[128] = "";
				zap_mutex_lock(chan_data->digit_mutex);
				chan_data->dtmf_len = zap_buffer_read(chan_data->digit_buffer, dtmf, sizeof(dtmf));
				pk_status = PKH_TG_PlayDTMF(chan_data->media_out, dtmf);
				zap_mutex_unlock(chan_data->digit_mutex);
			}
		}
		break;
	case PKH_EVENT_TG_TONE_PLAYED:
		{

			if (!event->p1) {
				zap_mutex_lock(chan_data->digit_mutex);
				PKH_PLAY_Start(chan_data->media_out);
				chan_data->dtmf_len = 0;
				zap_mutex_unlock(chan_data->digit_mutex);
			}

				
		}
		break;
	default:
		break;
	}

}


static unsigned pika_open_range(zap_span_t *span, unsigned boardno, unsigned spanno, unsigned start, unsigned end, 
								zap_chan_type_t type, char *name, char *number, pika_channel_profile_t *profile)
{
	unsigned configured = 0, x;
	PK_STATUS status;
	PK_CHAR error_text[PKH_ERROR_MAX_NAME_LENGTH];
	pika_span_data_t *span_data;

	if (boardno >= globals.board_list.numberOfBoards) {
		zap_log(ZAP_LOG_ERROR, "Board %u is not present!\n", boardno);
		return 0;
	}

	if (!globals.open_boards[boardno]) {
		status = PKH_BOARD_Open(globals.board_list.board[boardno].id,
								NULL,
								&globals.open_boards[boardno]);
		if(status != PK_SUCCESS) {
			zap_log(ZAP_LOG_ERROR, "Error: PKH_BOARD_Open %d failed(%s)!\n",  boardno,
					PKH_ERROR_GetText(status, error_text, sizeof(error_text)));
			return 0;
		}

		zap_log(ZAP_LOG_DEBUG, "Open board %u\n",  boardno);

		//PKH_BOARD_SetDebugTrace(globals.open_boards[boardno], 1, 0);
		
	}

	if (!span->mod_data) {
		span_data = malloc(sizeof(*span_data));
		assert(span_data != NULL);
		memset(span_data, 0, sizeof(*span_data));
		span_data->boardno = boardno;
		
		status = PKH_QUEUE_Create(PKH_QUEUE_TYPE_NORMAL, &span_data->event_queue);
		
		if (status != PK_SUCCESS) {
			zap_log(ZAP_LOG_ERROR, "Error: PKH_QUEUE_Create failed(%s)!\n",
					PKH_ERROR_GetText(status, error_text, sizeof(error_text)));
			free(span_data);
			return 0;
		}

		PKH_QUEUE_Attach(span_data->event_queue, globals.open_boards[boardno], NULL);

		span->mod_data = span_data;
	}
	start--;
	end--;
	for(x = start; x < end; x++) {
		zap_channel_t *chan;
		pika_chan_data_t *chan_data = NULL;

		chan_data = malloc(sizeof *chan_data);
		assert(chan_data);
		memset(chan_data, 0, sizeof(*chan_data));
		
		if (type == ZAP_CHAN_TYPE_FXO) {
			PKH_TTrunkConfig trunkConfig;
			
			if((status = PKH_TRUNK_Open(globals.open_boards[boardno], x, &chan_data->handle)) != PK_SUCCESS) {
				goto fail_fxo;
			}
		
			if ((status = PKH_TRUNK_Seize(chan_data->handle) != PK_SUCCESS)) {
				goto fail_fxo;
			}

			if (zap_span_add_channel(span, 0, type, &chan) != ZAP_SUCCESS) {
				goto fail_fxo;
			}

			if ((status = PKH_TRUNK_GetConfig(chan_data->handle, &trunkConfig) != PK_SUCCESS)) {
				goto fail_fxo;
			}
			
			trunkConfig.internationalControl = PKH_TRUNK_NA;
			trunkConfig.audioFormat = PKH_AUDIO_MULAW;

			if ((status = PKH_TRUNK_SetConfig(chan_data->handle, &trunkConfig) != PK_SUCCESS)) {
				goto fail_fxo;
			}
			
			if ((status = PKH_QUEUE_Attach(span_data->event_queue, chan_data->handle, (PK_VOID*) chan)) != PK_SUCCESS) {
				goto fail_fxo;
			}

			if ((status = PKH_TRUNK_GetMediaStreams(chan_data->handle, &chan_data->media_in, &chan_data->media_out)) != ZAP_SUCCESS) {
				goto fail_fxo;
			}

			if ((status = PKH_QUEUE_Create(PKH_QUEUE_TYPE_NORMAL, &chan_data->media_in_queue)) != PK_SUCCESS) {
				goto fail_fxo;
			}

			if ((status = PKH_QUEUE_Attach(chan_data->media_in_queue, chan_data->media_in, (PK_VOID*) chan)) != PK_SUCCESS) {
				goto fail_fxo;
			}

			if ((status = PKH_QUEUE_Create(PKH_QUEUE_TYPE_CALLBACK, &chan_data->media_out_queue)) != PK_SUCCESS) {
				goto fail_fxo;
			}

			if ((status = PKH_QUEUE_SetEventHandler(chan_data->media_out_queue, media_out_callback)) != PK_SUCCESS) {
				goto fail_fxo;
			}
		
			if ((status = PKH_QUEUE_Attach(chan_data->media_out_queue, chan_data->media_out, (PK_VOID*) chan)) != PK_SUCCESS) {
				goto fail_fxo;
			}
			
			if ((status = PKH_TRUNK_Start(chan_data->handle)) != PK_SUCCESS) {
				goto fail_fxo;
			}

			goto ok;

		fail_fxo:
			zap_log(ZAP_LOG_ERROR, "failure configuring device s%dc%d\n", spanno, x);
			zap_log(ZAP_LOG_ERROR, "Error: PKH_TRUNK_Open %u failed(%s)!\n", x,
					PKH_ERROR_GetText(status, error_text, sizeof(error_text)));
			if (chan_data->handle) {
				PKH_TRUNK_Close(chan_data->handle);
			}
			PKH_QUEUE_Destroy(chan_data->media_in_queue);

			free(chan_data);
			continue;
			
		} else if (type == ZAP_CHAN_TYPE_FXS) {
			if((status = PKH_PHONE_Open(globals.open_boards[boardno], x, &chan_data->handle)) != PK_SUCCESS) {
				goto fail_fxs;
			}
			
			if ((status = PKH_PHONE_Seize(chan_data->handle) != PK_SUCCESS)) {
				goto fail_fxs;
			}

			if (zap_span_add_channel(span, 0, type, &chan) != ZAP_SUCCESS) {
				goto fail_fxs;
			}
			
			if ((status = PKH_PHONE_GetMediaStreams(chan_data->handle, &chan_data->media_in, &chan_data->media_out)) != ZAP_SUCCESS) {
				goto fail_fxs;
			}
			
			if ((status = PKH_QUEUE_Attach(span_data->event_queue, chan_data->handle, (PK_VOID*) chan)) != PK_SUCCESS) {
				goto fail_fxs;
			}
			
			if ((status = PKH_QUEUE_Create(PKH_QUEUE_TYPE_NORMAL, &chan_data->media_in_queue)) != PK_SUCCESS) {
				goto fail_fxs;
			}

			if ((status = PKH_QUEUE_Attach(chan_data->media_in_queue, chan_data->media_in, (PK_VOID*) chan)) != PK_SUCCESS) {
				goto fail_fxs;
			}

			if ((status = PKH_QUEUE_Create(PKH_QUEUE_TYPE_CALLBACK, &chan_data->media_out_queue)) != PK_SUCCESS) {
				goto fail_fxs;
			}

			if ((status = PKH_QUEUE_SetEventHandler(chan_data->media_out_queue, media_out_callback)) != PK_SUCCESS) {
				goto fail_fxs;
			}
		
			if ((status = PKH_QUEUE_Attach(chan_data->media_out_queue, chan_data->media_out, (PK_VOID*) chan)) != PK_SUCCESS) {
				goto fail_fxs;
			}

			if ((status = PKH_PHONE_Start(chan_data->handle)) != PK_SUCCESS) {
				goto fail_fxs;
			}

			goto ok;


		fail_fxs:
			zap_log(ZAP_LOG_ERROR, "failure configuring device s%dc%d\n", spanno, x);
			zap_log(ZAP_LOG_ERROR, "Error: PKH_PHONE_Open %u failed(%s)!\n", x,
					PKH_ERROR_GetText(status, error_text, sizeof(error_text)));
			if (chan_data->handle) {
				PKH_PHONE_Close(chan_data->handle);
			}
			PKH_QUEUE_Destroy(chan_data->media_in_queue);
			free(chan_data);


		}

	ok:
		
		status = PKH_RECORD_GetConfig(chan_data->media_in, &chan_data->record_config);
		chan_data->record_config.encoding = PKH_RECORD_ENCODING_MU_LAW;
		chan_data->record_config.samplingRate = PKH_RECORD_SAMPLING_RATE_8KHZ;
		chan_data->record_config.bufferSize = PIKA_BLOCK_SIZE;
		chan_data->record_config.numberOfBuffers = chan_data->record_config.bufferSize;
		chan_data->record_config.VAD.enabled = PK_FALSE;
		//chan_data->record_config.speechSegmentEventsEnabled = PK_FALSE;
		//chan_data->record_config.gain = rxgain;



		status = PKH_PLAY_GetConfig(chan_data->media_out, &chan_data->play_config);
		chan_data->play_config.encoding = PKH_RECORD_ENCODING_MU_LAW;
		chan_data->play_config.samplingRate = PKH_RECORD_SAMPLING_RATE_8KHZ;
		chan_data->play_config.AGC.enabled = PK_FALSE;
		zap_log(ZAP_LOG_INFO, "configuring device b%ds%dc%d as OpenZAP device %d:%d\n", boardno, spanno, x, chan->span_id, chan->chan_id);

		if (profile) {
			zap_log(ZAP_LOG_INFO, "applying config profile %s to device %d:%d\n", profile->name, chan->span_id, chan->chan_id);
			chan_data->record_config.gain = profile->record_config.gain;
			chan_data->record_config.AGC = profile->record_config.AGC;
			chan_data->record_config.VAD = profile->record_config.VAD;
			chan_data->play_config.gain = profile->play_config.gain;
			chan_data->play_config.AGC = profile->play_config.AGC;
			chan_data->ec_enabled = profile->ec_enabled;
			chan_data->ec_config = profile->ec_config;
		}
		
		status = PKH_RECORD_SetConfig(chan_data->media_in, &chan_data->record_config);
		status = PKH_PLAY_SetConfig(chan_data->media_out, &chan_data->play_config);


		chan->mod_data = chan_data;
		chan->physical_span_id = spanno;
		chan->physical_chan_id = x;

		chan->rate = 8000;
		chan->packet_len = chan_data->record_config.bufferSize;
		chan->effective_interval = chan->native_interval = chan->packet_len / 8;

		PKH_RECORD_Start(chan_data->media_in);
		PKH_PLAY_Start(chan_data->media_out);
		if (chan_data->ec_enabled) {
			PKH_EC_Start(chan_data->media_in, chan_data->media_in, chan_data->media_out);
		}

		if (!zap_strlen_zero(name)) {
			zap_copy_string(chan->chan_name, name, sizeof(chan->chan_name));
		}
		
		if (!zap_strlen_zero(number)) {
			zap_copy_string(chan->chan_number, number, sizeof(chan->chan_number));
		}

		zap_channel_set_feature(chan, ZAP_CHANNEL_FEATURE_DTMF_GENERATE);
		zap_buffer_create(&chan_data->digit_buffer, 128, 128, 0);
		zap_mutex_create(&chan_data->digit_mutex);

		configured++;
	} 

	
	return configured;
}

static ZIO_CONFIGURE_SPAN_FUNCTION(pika_configure_span)
{
	int items, i;
    char *mydata, *item_list[10];
	char *bd, *sp, *ch, *mx;
	int boardno;
	int channo;
	int spanno;
	int top = 0;
	unsigned configured = 0;
	char *profile_name = NULL;
	pika_channel_profile_t *profile = NULL;

	assert(str != NULL);

	mydata = strdup(str);
	assert(mydata != NULL);

	if ((profile_name = strchr(mydata, '@'))) {
		*profile_name++ = '\0';
		if (!zap_strlen_zero(profile_name)) {
			profile = (pika_channel_profile_t *) hashtable_search(globals.profile_hash, (char *)profile_name);
		}
	}
		
	items = zap_separate_string(mydata, ',', item_list, (sizeof(item_list) / sizeof(item_list[0])));

	for(i = 0; i < items; i++) {
		bd = item_list[i];
		if ((sp = strchr(bd, ':'))) {
			*sp++ = '\0';
		}

		if ((ch = strchr(sp, ':'))) {
			*ch++ = '\0';
		}
		
		if (!(bd && sp && ch)) {
			zap_log(ZAP_LOG_ERROR, "Invalid input\n");
			continue;
		}

		boardno = atoi(bd);
		channo = atoi(ch);
		spanno = atoi(sp);


		if (boardno < 0) {
			zap_log(ZAP_LOG_ERROR, "Invalid board number %d\n", boardno);
			continue;
		}

		if (channo < 0) {
			zap_log(ZAP_LOG_ERROR, "Invalid channel number %d\n", channo);
			continue;
		}

		if (spanno < 0) {
			zap_log(ZAP_LOG_ERROR, "Invalid span number %d\n", channo);
			continue;
		}
		
		if ((mx = strchr(ch, '-'))) {
			mx++;
			top = atoi(mx) + 1;
		} else {
			top = channo + 1;
		}
		
		
		if (top < 0) {
			zap_log(ZAP_LOG_ERROR, "Invalid range number %d\n", top);
			continue;
		}

		configured += pika_open_range(span, boardno, spanno, channo, top, type, name, number, profile);

	}
	
	free(mydata);

	return configured;
}

static ZIO_OPEN_FUNCTION(pika_open) 
{
	pika_chan_data_t *chan_data = (pika_chan_data_t *) zchan->mod_data;
	PKH_QUEUE_Flush(chan_data->media_in_queue);
	PKH_PLAY_Start(chan_data->media_out);
	return ZAP_SUCCESS;
}

static ZIO_CLOSE_FUNCTION(pika_close)
{
	return ZAP_SUCCESS;
}

static ZIO_WAIT_FUNCTION(pika_wait)
{
	pika_chan_data_t *chan_data = (pika_chan_data_t *) zchan->mod_data;
	PK_STATUS status;
	zap_wait_flag_t myflags = *flags;
	PK_CHAR g_EventText[PKH_EVENT_MAX_NAME_LENGTH];

	*flags = ZAP_NO_FLAGS;	

	if (myflags & ZAP_READ) {
		status = PKH_QUEUE_WaitOnEvent(chan_data->media_in_queue, to, &chan_data->last_media_event);
		
		if (status == PK_SUCCESS) {
			if (chan_data->last_media_event.id == PKH_EVENT_QUEUE_TIMEOUT || chan_data->last_media_event.id == PKH_EVENT_RECORD_BUFFER_OVERFLOW) {
				return ZAP_TIMEOUT;
			}
			
			*flags |= ZAP_READ;
			return ZAP_SUCCESS;
		}

		PKH_EVENT_GetText(chan_data->last_media_event.id, g_EventText, sizeof(g_EventText));
		zap_log(ZAP_LOG_DEBUG, "Event: %s\n", g_EventText);
	}

	return ZAP_SUCCESS;
}

static ZIO_READ_FUNCTION(pika_read)
{
	pika_chan_data_t *chan_data = (pika_chan_data_t *) zchan->mod_data;
	PK_STATUS status;
	PK_CHAR g_EventText[PKH_EVENT_MAX_NAME_LENGTH];

	if (zchan->packet_len < *datalen) {
		*datalen = zchan->packet_len;
	}

	if ((status = PKH_RECORD_GetData(chan_data->media_in, data, *datalen)) == PK_SUCCESS) {
		return ZAP_SUCCESS;
	}


	PKH_ERROR_GetText(status, g_EventText, sizeof(g_EventText));
	zap_log(ZAP_LOG_DEBUG, "ERR: %s\n", g_EventText);
	return ZAP_FAIL;
}

static ZIO_WRITE_FUNCTION(pika_write)
{
	pika_chan_data_t *chan_data = (pika_chan_data_t *) zchan->mod_data;

	if (PKH_PLAY_AddData(chan_data->media_out, 0, data, *datalen) == PK_SUCCESS) {
		return ZAP_SUCCESS;
	}

	return ZAP_FAIL;
}

static ZIO_COMMAND_FUNCTION(pika_command)
{
	pika_chan_data_t *chan_data = (pika_chan_data_t *) zchan->mod_data;
	//pika_span_data_t *span_data = (pika_span_data_t *) zchan->span->mod_data;
	PK_STATUS pk_status;
	zap_status_t status = ZAP_SUCCESS;

	switch(command) {
	case ZAP_COMMAND_OFFHOOK:
		{
			if ((pk_status = PKH_TRUNK_SetHookSwitch(chan_data->handle, PKH_TRUNK_OFFHOOK)) != PK_SUCCESS) {
				PKH_ERROR_GetText(pk_status, zchan->last_error, sizeof(zchan->last_error));
				GOTO_STATUS(done, ZAP_FAIL);
			} else {
				zap_set_flag_locked(zchan, ZAP_CHANNEL_OFFHOOK);
			}
		}
		break;
	case ZAP_COMMAND_ONHOOK:
		{
			if ((pk_status = PKH_TRUNK_SetHookSwitch(chan_data->handle, PKH_TRUNK_ONHOOK)) != PK_SUCCESS) {
				PKH_ERROR_GetText(pk_status, zchan->last_error, sizeof(zchan->last_error));
				GOTO_STATUS(done, ZAP_FAIL);
			} else {
				zap_clear_flag_locked(zchan, ZAP_CHANNEL_OFFHOOK);
			}
		}
		break;
	case ZAP_COMMAND_GENERATE_RING_ON:
		{
			if ((pk_status = PKH_PHONE_RingStart(chan_data->handle, 0, 0)) != PK_SUCCESS) {
				PKH_ERROR_GetText(pk_status, zchan->last_error, sizeof(zchan->last_error));
				GOTO_STATUS(done, ZAP_FAIL);
			} else {
				zap_set_flag_locked(zchan, ZAP_CHANNEL_RINGING);
			}
		}
		break;
	case ZAP_COMMAND_GENERATE_RING_OFF:
		{
			if ((pk_status = PKH_PHONE_RingStop(chan_data->handle)) != PK_SUCCESS) {
				PKH_ERROR_GetText(pk_status, zchan->last_error, sizeof(zchan->last_error));
				GOTO_STATUS(done, ZAP_FAIL);
			} else {
				zap_clear_flag_locked(zchan, ZAP_CHANNEL_RINGING);
			}
		}
		break;
	case ZAP_COMMAND_GET_INTERVAL:
		{

			ZAP_COMMAND_OBJ_INT = zchan->native_interval;

		}
		break;
	case ZAP_COMMAND_SET_INTERVAL: 
		{
			int interval = ZAP_COMMAND_OBJ_INT;
			int len = interval * 8;
			chan_data->record_config.bufferSize = len;
			chan_data->record_config.numberOfBuffers = chan_data->record_config.bufferSize;
			zchan->packet_len = chan_data->record_config.bufferSize;
			zchan->effective_interval = zchan->native_interval = zchan->packet_len / 8;
			PKH_RECORD_SetConfig(chan_data->media_in, &chan_data->record_config);
			GOTO_STATUS(done, ZAP_SUCCESS);
		}
		break;
	case ZAP_COMMAND_GET_DTMF_ON_PERIOD:
		{

			ZAP_COMMAND_OBJ_INT = zchan->dtmf_on;
			GOTO_STATUS(done, ZAP_SUCCESS);

		}
		break;
	case ZAP_COMMAND_GET_DTMF_OFF_PERIOD:
		{			
				ZAP_COMMAND_OBJ_INT = zchan->dtmf_on;
				GOTO_STATUS(done, ZAP_SUCCESS);
		}
		break;
	case ZAP_COMMAND_SET_DTMF_ON_PERIOD:
		{
			int val = ZAP_COMMAND_OBJ_INT;
			if (val > 10 && val < 1000) {
				zchan->dtmf_on = val;
				GOTO_STATUS(done, ZAP_SUCCESS);
			} else {
				snprintf(zchan->last_error, sizeof(zchan->last_error), "invalid value %d range 10-1000", val);
				GOTO_STATUS(done, ZAP_FAIL);
			}
		}
		break;
	case ZAP_COMMAND_SET_DTMF_OFF_PERIOD:
		{
			int val = ZAP_COMMAND_OBJ_INT;
			if (val > 10 && val < 1000) {
				zchan->dtmf_off = val;
				GOTO_STATUS(done, ZAP_SUCCESS);
			} else {
				snprintf(zchan->last_error, sizeof(zchan->last_error), "invalid value %d range 10-1000", val);
				GOTO_STATUS(done, ZAP_FAIL);
			}
		}
		break;
	case ZAP_COMMAND_SEND_DTMF:
		{
			char *digits = ZAP_COMMAND_OBJ_CHAR_P;
			zap_log(ZAP_LOG_DEBUG, "Adding DTMF SEQ [%s]\n", digits);
			zap_mutex_lock(chan_data->digit_mutex);
			zap_buffer_write(chan_data->digit_buffer, digits, strlen(digits));
			zap_mutex_unlock(chan_data->digit_mutex);
			pk_status = PKH_PLAY_Stop(chan_data->media_out);
			
			if (pk_status != PK_SUCCESS) {
				PKH_ERROR_GetText(pk_status, zchan->last_error, sizeof(zchan->last_error));
				GOTO_STATUS(done, ZAP_FAIL);
			}
			GOTO_STATUS(done, ZAP_SUCCESS);
		}
		break;
	default:
		break;
	};

 done:
	return status;
}

static ZIO_SPAN_POLL_EVENT_FUNCTION(pika_poll_event)
{
	pika_span_data_t *span_data = (pika_span_data_t *) span->mod_data;
	PK_STATUS status;

	status = PKH_QUEUE_WaitOnEvent(span_data->event_queue, ms, &span_data->last_oob_event);

	if (status == PK_SUCCESS) {
		zap_channel_t *zchan = span_data->last_oob_event.userData;
		//PK_CHAR g_EventText[PKH_EVENT_MAX_NAME_LENGTH];

		if (span_data->last_oob_event.id == PKH_EVENT_QUEUE_TIMEOUT) {
			return ZAP_TIMEOUT;
		}
		
		//PKH_EVENT_GetText(span_data->last_oob_event.id, g_EventText, sizeof(g_EventText));
		//zap_log(ZAP_LOG_DEBUG, "Event: %s\n", g_EventText);

		if (zchan) {
			pika_chan_data_t *chan_data = (pika_chan_data_t *) zchan->mod_data;
			zap_set_flag(zchan, ZAP_CHANNEL_EVENT);
            zchan->last_event_time = zap_current_time_in_ms();
			chan_data->last_oob_event = span_data->last_oob_event;
		}

		return ZAP_SUCCESS;
	}

	return ZAP_FAIL;
}

static ZIO_SPAN_NEXT_EVENT_FUNCTION(pika_next_event)
{
	uint32_t i, event_id;
	
	for(i = 1; i <= span->chan_count; i++) {
		if (zap_test_flag((&span->channels[i]), ZAP_CHANNEL_EVENT)) {
			pika_chan_data_t *chan_data = (pika_chan_data_t *) span->channels[i].mod_data;
			PK_CHAR g_EventText[PKH_EVENT_MAX_NAME_LENGTH];
			
			zap_clear_flag((&span->channels[i]), ZAP_CHANNEL_EVENT);
			
			PKH_EVENT_GetText(chan_data->last_oob_event.id, g_EventText, sizeof(g_EventText));
			zap_log(ZAP_LOG_DEBUG, "Event %d on channel %d:%d [%s]\n", 
					chan_data->last_oob_event.id,
					span->channels[i].span_id,
					span->channels[i].chan_id,
					g_EventText);

			switch(chan_data->last_oob_event.id) {
			case PKH_EVENT_TRUNK_HOOKFLASH:
				event_id = ZAP_OOB_FLASH;
				break;
			case PKH_EVENT_TRUNK_RING_OFF:
				event_id = ZAP_OOB_RING_STOP;
				break;
			case PKH_EVENT_TRUNK_RING_ON:
				event_id = ZAP_OOB_RING_START;
				break;

			case PKH_EVENT_PHONE_OFFHOOK:
				zap_set_flag_locked((&span->channels[i]), ZAP_CHANNEL_OFFHOOK);
				event_id = ZAP_OOB_OFFHOOK;
				break;

			case PKH_EVENT_TRUNK_BELOW_THRESHOLD:
			case PKH_EVENT_TRUNK_ABOVE_THRESHOLD:
			case PKH_EVENT_PHONE_ONHOOK:
				zap_clear_flag_locked((&span->channels[i]), ZAP_CHANNEL_OFFHOOK);
				event_id = ZAP_OOB_ONHOOK;
				break;

			case PKH_EVENT_TRUNK_ONHOOK:
			case PKH_EVENT_TRUNK_OFFHOOK:				
			case PKH_EVENT_TRUNK_DIALED :
			case PKH_EVENT_TRUNK_REVERSAL:
			case PKH_EVENT_TRUNK_LCSO:
			case PKH_EVENT_TRUNK_DROPOUT:
			case PKH_EVENT_TRUNK_LOF:
			case PKH_EVENT_TRUNK_RX_OVERLOAD:
			default:
				zap_log(ZAP_LOG_DEBUG, "Unhandled event %d on channel %d [%s]\n", chan_data->last_oob_event.id, i, g_EventText);
				event_id = ZAP_OOB_INVALID;
				break;
			}

			span->channels[i].last_event_time = 0;
			span->event_header.e_type = ZAP_EVENT_OOB;
			span->event_header.enum_id = event_id;
			span->event_header.channel = &span->channels[i];
			*event = &span->event_header;
			return ZAP_SUCCESS;
		}
	}

	return ZAP_FAIL;
}

static ZIO_SPAN_DESTROY_FUNCTION(pika_span_destroy)
{
	pika_span_data_t *span_data = (pika_span_data_t *) span->mod_data;
	
	if (span_data) {
		PKH_QUEUE_Destroy(span_data->event_queue);
		free(span_data);
	}
	
	return ZAP_SUCCESS;
}

static ZIO_CHANNEL_DESTROY_FUNCTION(pika_channel_destroy)
{
	pika_chan_data_t *chan_data = (pika_chan_data_t *) zchan->mod_data;
	pika_span_data_t *span_data = (pika_span_data_t *) zchan->span->mod_data;

	if (zchan->type == ZAP_CHAN_TYPE_FXS || zchan->type == ZAP_CHAN_TYPE_FXO) {
		PKH_QUEUE_Detach(span_data->event_queue, chan_data->handle);
		PKH_TRUNK_Close(chan_data->handle);
	}

	PKH_RECORD_Stop(chan_data->media_in);
	PKH_PLAY_Stop(chan_data->media_out);
	PKH_QUEUE_Destroy(chan_data->media_in_queue);
	PKH_QUEUE_Destroy(chan_data->media_out_queue);
	zap_mutex_destroy(&chan_data->digit_mutex);
	zap_buffer_destroy(&chan_data->digit_buffer);
	zap_safe_free(chan_data);
	
	return ZAP_SUCCESS;
}

static ZIO_GET_ALARMS_FUNCTION(pika_get_alarms)
{
	return ZAP_FAIL;
}

static zap_io_interface_t pika_interface;

zap_status_t pika_init(zap_io_interface_t **zint)
{

	PK_STATUS status;
	PK_CHAR error_text[PKH_ERROR_MAX_NAME_LENGTH];
	uint32_t i;
	int ok = 0;
	PKH_TLogMasks m;


	assert(zint != NULL);
	memset(&pika_interface, 0, sizeof(pika_interface));
	memset(&globals, 0, sizeof(globals));

	globals.profile_hash = create_hashtable(16, zap_hash_hashfromstring, zap_hash_equalkeys);

	// Open the system object, to enumerate boards configured for this system
	if ((status = PKH_SYSTEM_Open(&globals.system_handle)) != PK_SUCCESS) {
		zap_log(ZAP_LOG_ERROR, "Error: PKH_SYSTEM_Open failed(%s)!\n",
				PKH_ERROR_GetText(status, error_text, sizeof(error_text)));
		return ZAP_FAIL;
	}

	// Retrieves a list of all boards in this system, existing,
	// or listed in pika.cfg
	if ((status = PKH_SYSTEM_Detect(globals.system_handle, &globals.board_list)) != PK_SUCCESS) {
		zap_log(ZAP_LOG_ERROR, "Error: PKH_SYSTEM_Detect failed(%s)!\n",
				PKH_ERROR_GetText(status, error_text, sizeof(error_text)));
		return ZAP_FAIL;
	}
	
	PKH_SYSTEM_GetConfig(globals.system_handle, &globals.system_config);
	globals.system_config.maxAudioProcessBlockSize = PIKA_BLOCK_LEN;
	globals.system_config.playBufferSize = PIKA_BLOCK_SIZE;
	globals.system_config.recordBufferSize = PIKA_BLOCK_SIZE;
	globals.system_config.recordNumberOfBuffers = 8;
	PKH_SYSTEM_SetConfig(globals.system_handle, &globals.system_config);

	zap_log(ZAP_LOG_DEBUG, "Found %u board%s\n", globals.board_list.numberOfBoards, globals.board_list.numberOfBoards == 1 ? "" : "s");
	for(i = 0; i < globals.board_list.numberOfBoards; ++i) {
		zap_log(ZAP_LOG_INFO, "Found PIKA board type:[%s] id:[%u] serno:[%u]\n", 
				pika_board_type_string(globals.board_list.board[i].type), globals.board_list.board[i].id, (uint32_t)
				globals.board_list.board[i].serialNumber);

		ok++;

	}

	if (!ok) {
		return ZAP_FAIL;
	}
	
	pika_interface.name = "pika";
	pika_interface.configure =  pika_configure;
	pika_interface.configure_span = pika_configure_span;
	pika_interface.open = pika_open;
	pika_interface.close = pika_close;
	pika_interface.wait = pika_wait;
	pika_interface.read = pika_read;
	pika_interface.write = pika_write;
	pika_interface.command = pika_command;
	pika_interface.poll_event = pika_poll_event;
	pika_interface.next_event = pika_next_event;
	pika_interface.channel_destroy = pika_channel_destroy;
	pika_interface.span_destroy = pika_span_destroy;
	pika_interface.get_alarms = pika_get_alarms;
	*zint = &pika_interface;

	
	

	memset(&m, 0, sizeof(m));
	//m.apiMask = 0xffffffff;
	//PKH_LOG_SetMasks(&m);

	return ZAP_SUCCESS;
}

zap_status_t pika_destroy(void)
{
	uint32_t x;
	PK_STATUS status;
	PK_CHAR error_text[PKH_ERROR_MAX_NAME_LENGTH];

	for (x = 0; x < MAX_NUMBER_OF_TRUNKS; x++) {
		if (globals.open_boards[x]) {
			zap_log(ZAP_LOG_INFO, "Closing board %u\n", x);
			PKH_BOARD_Close(globals.open_boards[x]);
		}
	}

	// The system can now be closed.
	if ((status = PKH_SYSTEM_Close(globals.system_handle)) != PK_SUCCESS) {
		zap_log(ZAP_LOG_ERROR, "Error: PKH_SYSTEM_Close failed(%s)!\n",
				PKH_ERROR_GetText(status, error_text, sizeof(error_text)));
	}

	hashtable_destroy(globals.profile_hash, 0, 1);

	return ZAP_SUCCESS;
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 expandtab:
 */

