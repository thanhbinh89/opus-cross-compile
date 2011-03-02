/* Copyright (c) 2010 Xiph.Org Foundation, Skype Limited
   Written by Jean-Marc Valin and Koen Vos */
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include "opus_decoder.h"
#include "entdec.h"
#include "modes.h"
#include "SKP_Silk_SDK_API.h"


OpusDecoder *opus_decoder_create(int Fs, int channels)
{
    char *raw_state;
	int ret, silkDecSizeBytes, celtDecSizeBytes;
	OpusDecoder *st;

	/* Initialize SILK encoder */
    ret = SKP_Silk_SDK_Get_Decoder_Size( &silkDecSizeBytes );
    if( ret ) {
        /* Handle error */
    }
    celtDecSizeBytes = celt_decoder_get_size(channels);
    raw_state = calloc(sizeof(OpusDecoder)+silkDecSizeBytes+celtDecSizeBytes, 1);
    st = (OpusDecoder*)raw_state;
    st->silk_dec = (void*)(raw_state+sizeof(OpusDecoder));
    st->celt_dec = (CELTDecoder*)(raw_state+sizeof(OpusDecoder)+silkDecSizeBytes);
    st->stream_channels = st->channels = channels;

    st->Fs = Fs;

    /* Reset decoder */
    ret = SKP_Silk_SDK_InitDecoder( st->silk_dec );
    if( ret ) {
        /* Handle error */
    }

	/* Initialize CELT decoder */
	st->celt_dec = celt_decoder_init(st->celt_dec, Fs, channels, NULL);

	st->prev_mode = 0;
	return st;
}

int opus_decode(OpusDecoder *st, const unsigned char *data,
		int len, short *pcm, int frame_size, int decode_fec)
{
	int i, silk_ret=0, celt_ret=0;
	ec_dec dec;
    SKP_SILK_SDK_DecControlStruct DecControl;
    SKP_int32 silk_frame_size;
    short pcm_celt[960*2];
    short pcm_transition[960*2];
    int audiosize;
    int mode;
    int transition=0;
    int start_band;
    int redundancy;

    /* Payloads of 1 (2 including ToC) or 0 trigger the PLC/DTX */
    if (len<=2)
    	data = NULL;

    if (data != NULL)
    {
        /* Decoding mode/bandwidth/framesize from first byte */
        if (data[0]&0x80)
        {
            mode = MODE_CELT_ONLY;
            st->bandwidth = BANDWIDTH_MEDIUMBAND + ((data[0]>>5)&0x3);
            if (st->bandwidth == BANDWIDTH_MEDIUMBAND)
                st->bandwidth = BANDWIDTH_NARROWBAND;
            audiosize = ((data[0]>>3)&0x3);
            audiosize = (st->Fs<<audiosize)/400;
        } else if ((data[0]&0x60) == 0x60)
        {
            mode = MODE_HYBRID;
            st->bandwidth = (data[0]&0x10) ? BANDWIDTH_FULLBAND : BANDWIDTH_SUPERWIDEBAND;
            audiosize = (data[0]&0x08) ? st->Fs/50 : st->Fs/100;
        } else {

            mode = MODE_SILK_ONLY;
            st->bandwidth = BANDWIDTH_NARROWBAND + ((data[0]>>5)&0x3);
            audiosize = ((data[0]>>3)&0x3);
            if (audiosize == 3)
                audiosize = st->Fs*60/1000;
            else
                audiosize = (st->Fs<<audiosize)/100;
        }
        st->stream_channels = (data[0]&0x4) ? 2 : 1;
        /*printf ("%d %d %d\n", st->mode, st->bandwidth, audiosize);*/

        len -= 1;
        data += 1;
        ec_dec_init(&dec,(unsigned char*)data,len);
    } else {
        audiosize = frame_size;
        mode = st->prev_mode;
    }

    if (mode != st->prev_mode && st->prev_mode > 0
    		&& !(mode == MODE_SILK_ONLY && st->prev_mode == MODE_HYBRID)
    		&& !(mode == MODE_HYBRID && st->prev_mode == MODE_SILK_ONLY))
    {
    	transition = 1;
    	if (mode == MODE_CELT_ONLY && !st->prev_redundancy)
    	    opus_decode(st, NULL, 0, pcm_transition, IMAX(480, audiosize), 0);
    }
    if (audiosize > frame_size)
    {
        fprintf(stderr, "PCM buffer too small");
        return -1;
    } else {
        frame_size = audiosize;
    }

    /* SILK processing */
    if (mode != MODE_CELT_ONLY)
    {
        int lost_flag, decoded_samples;
        SKP_int16 *pcm_ptr = pcm;

        if (st->prev_mode==MODE_CELT_ONLY)
        	SKP_Silk_SDK_InitDecoder( st->silk_dec );

        DecControl.API_sampleRate = st->Fs;
        DecControl.payloadSize_ms = 1000 * audiosize / st->Fs;
        if( mode == MODE_SILK_ONLY ) {
            if( st->bandwidth == BANDWIDTH_NARROWBAND ) {
                DecControl.internalSampleRate = 8000;
            } else if( st->bandwidth == BANDWIDTH_MEDIUMBAND ) {
                DecControl.internalSampleRate = 12000;
            } else if( st->bandwidth == BANDWIDTH_WIDEBAND ) {
                DecControl.internalSampleRate = 16000;
            } else {
                SKP_assert( 0 );
            }
        } else {
            /* Hybrid mode */
            DecControl.internalSampleRate = 16000;
        }

        lost_flag = data == NULL ? 1 : 2 * decode_fec;
        decoded_samples = 0;
        do {
            /* Call SILK decoder */
            int first_frame = decoded_samples == 0;
            silk_ret = SKP_Silk_SDK_Decode( st->silk_dec, &DecControl, 
                lost_flag, first_frame, &dec, len, pcm_ptr, &silk_frame_size );
            if( silk_ret ) {
                fprintf (stderr, "SILK decode error\n");
                /* Handle error */
            }
            pcm_ptr += silk_frame_size;
            decoded_samples += silk_frame_size;
        } while( decoded_samples < frame_size );
    } else {
        for (i=0;i<frame_size*st->channels;i++)
            pcm[i] = 0;
    }

    start_band = 0;
    if (mode == MODE_HYBRID)
    {
        /* Check if we have a redundant 0-8 kHz band */
        redundancy = ec_dec_bit_logp(&dec, 12);
        if (!redundancy)
            start_band = 17;
    }
    celt_decoder_ctl(st->celt_dec, CELT_SET_START_BAND(start_band));

    if (redundancy)
        transition = 0;

    if (transition && mode != MODE_CELT_ONLY)
        opus_decode(st, NULL, 0, pcm_transition, IMAX(480, audiosize), 0);

    if (mode != MODE_SILK_ONLY)
    {
    	int endband;

	    switch(st->bandwidth)
	    {
	    case BANDWIDTH_NARROWBAND:
	    	endband = 13;
	    	break;
	    case BANDWIDTH_WIDEBAND:
	    	endband = 17;
	    	break;
	    case BANDWIDTH_SUPERWIDEBAND:
	    	endband = 19;
	    	break;
	    case BANDWIDTH_FULLBAND:
	    	endband = 21;
	    	break;
	    }
	    celt_decoder_ctl(st->celt_dec, CELT_SET_END_BAND(endband));
	    celt_decoder_ctl(st->celt_dec, CELT_SET_CHANNELS(st->stream_channels));

	    if (st->prev_mode == MODE_SILK_ONLY)
	    	celt_decoder_ctl(st->celt_dec, CELT_RESET_STATE);
        /* Decode CELT */
        celt_ret = celt_decode_with_ec(st->celt_dec, decode_fec?NULL:data, len, pcm_celt, frame_size, &dec);
        for (i=0;i<frame_size*st->channels;i++)
            pcm[i] = ADD_SAT16(pcm[i], pcm_celt[i]);
    }

    if (transition)
    {
    	int plc_length, overlap;
    	if (mode == MODE_CELT_ONLY)
    		plc_length = IMIN(audiosize, 10+st->Fs/200);
    	else
    		plc_length = IMIN(audiosize, 10+st->Fs/400);
    	for (i=0;i<plc_length;i++)
    		pcm[i] = pcm_transition[i];

    	overlap = IMIN(480, IMAX(0, audiosize-plc_length));
    	for (i=0;i<overlap;i++)
    		pcm[plc_length+i] = (i*pcm[plc_length+i] + (overlap-i)*pcm_transition[plc_length+i])/overlap;
    }
#if OPUS_TEST_RANGE_CODER_STATE
    st->rangeFinal = dec.rng;
#endif

    st->prev_mode = mode;
    st->prev_redundancy = redundancy;
	return celt_ret<0 ? celt_ret : audiosize;

}

void opus_decoder_ctl(OpusDecoder *st, int request, ...)
{
    va_list ap;

    va_start(ap, request);

    switch (request)
    {
        case OPUS_GET_MODE_REQUEST:
        {
            int *value = va_arg(ap, int*);
            *value = st->prev_mode;
        }
        break;
        case OPUS_SET_BANDWIDTH_REQUEST:
        {
            int value = va_arg(ap, int);
            st->bandwidth = value;
        }
        break;
        case OPUS_GET_BANDWIDTH_REQUEST:
        {
            int *value = va_arg(ap, int*);
            *value = st->bandwidth;
        }
        break;
        default:
            fprintf(stderr, "unknown opus_decoder_ctl() request: %d", request);
            break;
    }

    va_end(ap);
}

void opus_decoder_destroy(OpusDecoder *st)
{
	free(st);
}

#if OPUS_TEST_RANGE_CODER_STATE
int opus_decoder_get_final_range(OpusDecoder *st)
{
    return st->rangeFinal;
}
#endif
