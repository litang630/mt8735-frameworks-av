/*
**
** Copyright 2007, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#define LOG_TAG "AudioMTKMixer"
#define LOG_NDEBUG 0

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>

#include <utils/Errors.h>
#include <utils/Log.h>

#include <cutils/bitops.h>
#include <cutils/compiler.h>
#include <utils/Debug.h>

#include <system/audio.h>

#include <audio_utils/primitives.h>
#include <common_time/local_clock.h>
#include <common_time/cc_helper.h>

#include <media/EffectsFactoryApi.h>

#include "AudioMTKMixer.h"

#include <cutils/xlog.h>

#include "AudioCompensationFilter.h"

#ifdef DEBUG_MIXER_PCM
#include "AudioUtilmtk.h"
#endif
#ifdef TIME_STRETCH_ENABLE
#include "AudioMTKTimeStretch.h"
#endif

#include <media/AudioSystem.h>
#include <AudioPolicyParameters.h>

#include "AudioMTKHardwareCommand.h"

#define MTK_ALOG_V(fmt, arg...) SXLOGV(fmt, ##arg)
#define MTK_ALOG_D(fmt, arg...) SXLOGD(fmt, ##arg)
#define MTK_ALOG_W(fmt, arg...) SXLOGW(fmt, ##arg)
#define MTK_ALOG_E(fmt, arg...) SXLOGE("Err: %5d:, "fmt, __LINE__, ##arg)
#undef  ALOGV
#define ALOGV   MTK_ALOG_V

#ifdef MTK_AUDIO
#ifdef DEBUG_AUDIO_PCM
    static   const char * gaf_timestretch_in_pcm = "/sdcard/mtklog/audio_dump/af_mixer_timestretch_in.pcm";
    static   const char * gaf_timestretch_in_propty = "af.timestretch.in.pcm";
#endif
#endif

namespace android {
//-----------------------------------------------------------------------------
#ifdef TIME_STRETCH_ENABLE
AudioMTKMixer::TimeStretchBufferProvider::TimeStretchBufferProvider(int framecount) : AudioBufferProvider(),
        mTrackBufferProvider(NULL), mTimeStretchHandle(NULL), mOutBuffer(NULL), mOutRemain(0)
{
	if(framecount >0)
	{
		mOutframecount = framecount;
	}
	else
		mOutframecount = 4096; 
         ALOGV("new Timestretch, internal input buffer framecount %d ",mOutframecount );
	mTimeStretchHandle = new AudioMTKTimeStretch(mOutframecount);
	mOutBuffer = new short[mOutframecount*4]; // *2 for channel count, *2 for downsampling.
}
AudioMTKMixer::TimeStretchBufferProvider::~TimeStretchBufferProvider()
{
    //ALOGV("AudioMTKMixer deleting TimeStretchBufferProvider (%p)", this);
	if(mTimeStretchHandle !=NULL)
	{
		delete mTimeStretchHandle;
	}
	if(mOutBuffer != NULL)
	{
		delete []mOutBuffer;
	}
}
void AudioMTKMixer::TimeStretchBufferProvider::releaseBuffer(AudioBufferProvider::Buffer *pBuffer) {
	
	int ChannelCount = popcount(mTimeStretchConfig.inputCfg.channels);
	ALOGD("TimeStretchBufferProvider::releaseBuffer()");
	
	if(pBuffer ==NULL)
	{
		ALOGE("DownmixerBufferProvider::releaseBuffer() error: NULL track buffer provider");
	}
	if(mOutRemain  == 0 && pBuffer->frameCount != 0)
	{
		// for frame count < 512,  time stretch is not activated, so getNextBuffer returns non-time stretched buffer
		// and release non time stretched buffer here.
		ALOGD("for in frame count <512 case realease real buffer (non-stretched) count");
		mBuffer.frameCount = pBuffer->frameCount;
		mTrackBufferProvider->releaseBuffer(&mBuffer);
	}
	else{
		// maintain internal buffer: internal buffer(mOutBuffer) keeps time stretched data. 
		ALOGD("release pBuffer-> raw %x, pBuffer->frameCount %d",pBuffer->raw ,pBuffer->frameCount  );
		mOutRemain -= pBuffer->frameCount ;	
		memcpy(mOutBuffer, mOutBuffer + (pBuffer->frameCount * ChannelCount),(mOutRemain* ChannelCount)*sizeof(short) );
		//mBuffer.raw = pBuffer->raw;
		ALOGV("release mBuffer-> raw %x, mBuffer->frameCount %d",mBuffer.raw ,mBuffer.frameCount  );
		// release what we originally get from audio Track.
		mTrackBufferProvider->releaseBuffer(&mBuffer);
	}
	pBuffer->frameCount = mOutRemain;	
	pBuffer->raw = mOutBuffer;
	ALOGD("TimeStretchBufferProvider %d keeped.",mOutRemain);
	ALOGD("release pBuffer-> raw %x, pBuffer->frameCount %d",pBuffer->raw ,pBuffer->frameCount  );
}
status_t AudioMTKMixer::TimeStretchBufferProvider::getNextBuffer(AudioBufferProvider::Buffer *pBuffer,
int64_t pts) {
	int  outsize; 
	int insize;	
	int ChannelCount;
	short* OutBufferPtr;
	status_t res;
	ALOGD("TimeStretchBufferProvider::getNextBuffer()");

	if (this->mTrackBufferProvider == NULL ) { 
		ALOGE("TimeStretchBufferProvider::getNextBuffer() error: NULL track buffer provider");
		return NO_INIT;
	}
	if( mOutBuffer == NULL){
		
		ALOGE("TimeStretchBufferProvider::getNextBuffer() error: NULL internal buffer");
		return NO_INIT;
	}
	if(mOutRemain !=0)
	{
		// if internal buffer still has time stretched data, return directly.
		ALOGD("TimeStretchBufferProvider::getNextBuffer() directly return");
		pBuffer->frameCount = mOutRemain;
		pBuffer->raw = mOutBuffer;
		return OK;
	}

	/////////////// Get new data and process///////////////////////////
	
	ALOGV("mOutframecount%d, pBuffer->frameCount %d",mOutframecount,pBuffer->frameCount);

	////////////Check buffer size availability//////////////////////////////
	if (mOutframecount < pBuffer->frameCount)
	{
		pBuffer->frameCount = mOutframecount; // can't exceed internal buffer size;
	}
	
	//pBuffer->frameCount = pBuffer->frameCount -mOutRemain;
	ALOGD(" pBuffer->frameCount  %d",pBuffer->frameCount);

	/////////// Calculate needed input frame count//////////////////////////
	if(mTimeStretchHandle->mBTS_RTParam.TS_Ratio == 100 || mTimeStretchHandle ==NULL){
		pBuffer->frameCount = pBuffer->frameCount;
	}else{
		pBuffer->frameCount = (pBuffer->frameCount*100)/mTimeStretchHandle->mBTS_RTParam.TS_Ratio ;
                pBuffer->frameCount = (pBuffer->frameCount == 0)?(pBuffer->frameCount+1) : pBuffer->frameCount;
	}
          pBuffer->frameCount = mTimeStretchHandle->InternalBufferSpace() > pBuffer->frameCount ? pBuffer->frameCount: mTimeStretchHandle->InternalBufferSpace();
          if(mTimeStretchHandle->mBTS_RTParam.TS_Ratio <= 400 && mTimeStretchHandle->mBTS_RTParam.TS_Ratio > 100){
          if(mTimeStretchHandle->InternalBufferFrameCount() + pBuffer->frameCount  <256)
          {// require more frame so that time stretch can be motivated.
            pBuffer->frameCount = 256-mTimeStretchHandle->InternalBufferFrameCount();
              }
          }
	/////////Get data////////////////////////////////////////////////
	ALOGD("Timestertch getNextBuffer real required%d", pBuffer->frameCount );
	mBuffer.frameCount = pBuffer->frameCount;   	
	mBuffer.raw = pBuffer->raw;   
	res = mTrackBufferProvider->getNextBuffer(&mBuffer, pts);
	
	ChannelCount = popcount(mTimeStretchConfig.inputCfg.channels);
#ifdef MTK_AUDIO
#ifdef DEBUG_AUDIO_PCM
			const int SIZE = 256;
			char fileName[SIZE];
			sprintf(fileName,"%s_%p.pcm",gaf_timestretch_in_pcm,this);
			AudioDump::dump(fileName,mBuffer.raw,mBuffer.frameCount<<ChannelCount,gaf_timestretch_in_propty);
#endif
#endif
	//ALOGD("mBuffer.raw %x,mBuffer.frameCount*4 %d",mBuffer.raw,mBuffer.frameCount<<ChannelCount);
	ALOGD("Timestertch getNextBuffer real get%d", mBuffer.frameCount );   

	#if  1
	////////////////////process data///////////////////////////////////
	if (res == OK && mBuffer.frameCount  !=0) {
		insize = mBuffer.frameCount << ChannelCount;

		// available output buffer space
		outsize = (mOutframecount -mOutRemain) << ChannelCount;

		// output pointer offset to last round data
		OutBufferPtr = mOutBuffer + (mOutRemain << (ChannelCount-1));

		mTimeStretchHandle->process(mBuffer.i16,OutBufferPtr ,&insize, &outsize );
		// insize always returns 0, since timestretch accumulate samples.
		//ALOGV("getNextBuffer is TimeStretching"); 
		mOutRemain += outsize >> (ChannelCount); 
		// real consumed sample count, release later.
		//mBuffer.frameCount -= ( insize >> ChannelCount); //(stereo : 2, mono :1)
		#if 0
		if(mOutRemain != 0)
		{
			// stretched output framecount 
			pBuffer->frameCount = mOutRemain;
			// replace out buffer.
			pBuffer->raw = mOutBuffer;
		}
		else{			
			/// for smple less than 512 sample, do not do time stretch, returns original getNextBuffer sample count.
			/// use orignal  buffer frame count and buffer size to bypass time stretch.
			pBuffer->frameCount = mBuffer.frameCount;
			pBuffer->raw = mBuffer.raw;
		}
		#else
        
                    // stretched output framecount 
                    pBuffer->frameCount = mOutRemain;
                    // replace out buffer.
                    pBuffer->raw = mOutBuffer;
        #endif
		ALOGD(" getNextBuffer: mOutRemain %d", mOutRemain);
	}
	else
	{
		ALOGD("getNexBuffer returns not ok");
		pBuffer->frameCount  = mBuffer.frameCount ;
		pBuffer->raw = mBuffer.raw;
	}
	#else
	//memcpy(mOutBuffer, mBuffer.i16, mBuffer.frameCount*4);
	
	ALOGD("mBuffer-> raw %x, mBuffer->frameCount %d",mBuffer.raw ,mBuffer.frameCount  );
	//pBuffer->raw = mBuffer.raw;
	pBuffer->raw = mOutBuffer;
	pBuffer->frameCount = mBuffer.frameCount;
	//mBuffer.raw = mOutBuffer;
	//mBuffer.frameCount = pBuffer->frameCount;
	
	ALOGD("pBuffer-> raw %x, pBuffer->frameCount %d",pBuffer->raw ,pBuffer->frameCount  );
	#endif
return res;
}

status_t AudioMTKMixer::TimeStretchBufferProvider::TimeStretchConfig(int ratio) 
{
	int ch_num;
	if(mTimeStretchHandle == NULL)
		return -1;
	if(mTimeStretchConfig.inputCfg.samplingRate == 0 || 
		(mTimeStretchConfig.inputCfg.channels & (!AUDIO_CHANNEL_OUT_STEREO)) )
		return -1;
	
	if(mTimeStretchConfig.inputCfg.format != AUDIO_FORMAT_PCM_16_BIT )
		return -1;
	ch_num = (mTimeStretchConfig.inputCfg.channels & AUDIO_CHANNEL_OUT_MONO) ? 1: 2;
	mTimeStretchHandle->setParameters(&ratio);
        return OK;
}
#endif
#ifdef DEBUG_MIXER_PCM
static   const char * gaf_mixer_dcremove_pcm    = "/sdcard/mtklog/audio_dump/mixer_dcremove";
static   const char * gaf_mixer_dcremove_propty = "af.mixer.dcremove.pcm";

static   const char * gaf_mixer_drc_pcm         = "/sdcard/mtklog/audio_dump/mixer_drc";
static   const char * gaf_mixer_drc_propty      = "af.mixer.drc.pcm";

static   const char * gaf_mixer_gain_pcm        = "/sdcard/mtklog/audio_dump/mixer_gain";
static   const char * gaf_mixer_gain_propty     = "af.mixer.gain.pcm";

static   const char * gaf_mixer_limin_pcm       = "/sdcard/mtklog/audio_dump/mixer_limin";
static   const char * gaf_mixer_limin_propty    = "af.mixer.limin.pcm";

#define MixerDumpPcm(name, propty, tid, value, buffer, size) \
{\
  char fileName[256]; \
  sprintf(fileName,"%s_%d_%p.pcm", name, tid, value); \
  AudioDump::dump(fileName, buffer, size, propty); \
}
#else
#define MixerDumpPcm(name, propty, tid, value, buffer, size)
#endif

bool AudioMTKMixer::mUIDRCEnable = false;

// ----------------------------------------------------------------------------
AudioMTKMixer::DownmixerBufferProvider::DownmixerBufferProvider() : AudioBufferProvider(),
        mTrackBufferProvider(NULL), mDownmixHandle(NULL)
{
}

AudioMTKMixer::DownmixerBufferProvider::~DownmixerBufferProvider()
{
    ALOGV("AudioMTKMixer deleting DownmixerBufferProvider (%p)", this);
    EffectRelease(mDownmixHandle);
}

status_t AudioMTKMixer::DownmixerBufferProvider::getNextBuffer(AudioBufferProvider::Buffer *pBuffer,
        int64_t pts) {
    //ALOGV("DownmixerBufferProvider::getNextBuffer()");
    if (this->mTrackBufferProvider != NULL) {
        status_t res = mTrackBufferProvider->getNextBuffer(pBuffer, pts);
        if (res == OK) {
            mDownmixConfig.inputCfg.buffer.frameCount = pBuffer->frameCount;
            mDownmixConfig.inputCfg.buffer.raw = pBuffer->raw;
            mDownmixConfig.outputCfg.buffer.frameCount = pBuffer->frameCount;
            mDownmixConfig.outputCfg.buffer.raw = mDownmixConfig.inputCfg.buffer.raw;
            // in-place so overwrite the buffer contents, has been set in prepareTrackForDownmix()
            //mDownmixConfig.outputCfg.accessMode = EFFECT_BUFFER_ACCESS_WRITE;

            res = (*mDownmixHandle)->process(mDownmixHandle,
                    &mDownmixConfig.inputCfg.buffer, &mDownmixConfig.outputCfg.buffer);
            //ALOGV("getNextBuffer is downmixing");
        }
        return res;
    } else {
        ALOGE("DownmixerBufferProvider::getNextBuffer() error: NULL track buffer provider");
        return NO_INIT;
    }
}

void AudioMTKMixer::DownmixerBufferProvider::releaseBuffer(AudioBufferProvider::Buffer *pBuffer) {
    //ALOGV("DownmixerBufferProvider::releaseBuffer()");
    if (this->mTrackBufferProvider != NULL) {
        mTrackBufferProvider->releaseBuffer(pBuffer);
    } else {
        ALOGE("DownmixerBufferProvider::releaseBuffer() error: NULL track buffer provider");
    }
}

// -- UI Dynamic Control DRC --------------------------------------------------------------------------

static AudioMTKMixer *MixerInstance = NULL;

void DRCCallback(void *data)
{
    ALOGD("DRCCallback %d", (bool)data);
    if (MixerInstance != NULL)
    {
        MixerInstance->setDRCEnable((bool)data);
    }
}

void SetDRCCallback(void *data)
{
    if(MixerInstance)
        return;
    
    MixerInstance = (AudioMTKMixer *)data;
    BESLOUDNESS_CONTROL_CALLBACK_STRUCT callback_data;
    callback_data.callback = DRCCallback;
    AudioSystem::SetAudioData(HOOK_BESLOUDNESS_CONTROL_CALLBACK, 0, &callback_data);
}

// ----------------------------------------------------------------------------
bool AudioMTKMixer::isMultichannelCapable = false;
#ifdef TIME_STRETCH_ENABLE
bool AudioMTKMixer::isTimeStretchCapable = true;
#endif
effect_descriptor_t AudioMTKMixer::dwnmFxDesc;

// Ensure mConfiguredNames bitmask is initialized properly on all architectures.
// The value of 1 << x is undefined in C when x >= 32.

AudioMTKMixer::AudioMTKMixer(size_t frameCount, uint32_t sampleRate, uint32_t maxNumTracks)
    :   mTrackNames(0), mConfiguredNames((maxNumTracks >= 32 ? 0 : 1 << maxNumTracks) - 1),
        mSampleRate(sampleRate)
{
    MTK_ALOG_V("AudioFlinger::MixerThread:  constructor mSampleRate = %d",mSampleRate);
    // AudioMTKMixer is not yet capable of multi-channel beyond stereo
    COMPILE_TIME_ASSERT_FUNCTION_SCOPE(2 == MAX_NUM_CHANNELS);

    ALOG_ASSERT(maxNumTracks <= MAX_NUM_TRACKS, "maxNumTracks %u > MAX_NUM_TRACKS %u",
            maxNumTracks, MAX_NUM_TRACKS);

    LocalClock lc;

    mState.enabledTracks= 0;
    mState.needsChanged = 0;
    mState.frameCount   = frameCount;
    mState.hook         = process__nop;
    mState.outputTemp   = NULL;
    mState.resampleTemp = NULL;
    mState.aggregationBuffer = NULL;
    mState.aggregationCount = 0;
#ifndef MTK_BESSURROUND_ENABLE
    mState.resampleTemp = new int32_t[MAX_NUM_CHANNELS * mState.frameCount];
#else
    mState.downMixBuffer = new int32_t[MAX_NUM_CHANNELS*mState.frameCount]; 
    mState.resampleTemp = new int32_t[MAX_NUM_CHANNELS_TO_DOWNMIX * mState.frameCount];
#endif
    mState.outputTemp = new int64_t[MAX_NUM_CHANNELS * mState.frameCount];
    mState.aggregationBuffer = new int32_t[MAX_NUM_CHANNELS * mState.frameCount];


#ifdef MTK_AUDIOMIXER_ENABLE_DRC
    mState.mDRCSupport = false;
    mState.mDRCEnable = false;
    mState.pDRCTempBuffer = new int32_t[MAX_NUM_CHANNELS * mState.frameCount];
#endif

#ifdef MTK_AUDIOMIXER_ENABLE_LIMITER
    {
        uint32_t intBufSize, tempBufSize;
        Limiter_InitParam stLimitParam;
        
        Limiter_GetBufferSize( &intBufSize, &tempBufSize, LMTR_IN_Q33P31_OUT_Q1P31);
        
        mState.pLimiterInternalBuffer = new uint8_t[intBufSize];
        mState.pLimiterTempBuffer = new uint8_t[tempBufSize];
        
        stLimitParam.Channel = 2;
        stLimitParam.Sampling_Rate = sampleRate;
        stLimitParam.PCM_Format = LMTR_IN_Q33P31_OUT_Q1P31;
        Limiter_Open( &mState.mpLimiterObj, mState.pLimiterInternalBuffer, &stLimitParam);
        
    }
#endif

    // mState.reserved

    // FIXME Most of the following initialization is probably redundant since
    // tracks[i] should only be referenced if (mTrackNames & (1 << i)) != 0
    // and mTrackNames is initially 0.  However, leave it here until that's verified.
    track_t* t = mState.tracks;
    for (unsigned i=0 ; i < MAX_NUM_TRACKS ; i++) {
        // FIXME redundant per track
        t->localTimeFreq = lc.getLocalFreq();
        t->resampler = NULL;
        t->downmixerBufferProvider = NULL;
#ifdef TIME_STRETCH_ENABLE
        t->timestretchBufferProvider = NULL;
#endif

#ifdef MTK_AUDIOMIXER_ENABLE_DRC
        t->mDRCEnable = false;
        t->mDRCState = false;
        t->mpDRCObj = NULL;
#endif
#ifdef MTK_BESSURROUND_ENABLE
        t->mSurroundMixer = NULL;
        t->mSurroundEnable = 0;
        t->mSurroundMode = 0;
#endif
        t++;
    }

    // find multichannel downmix effect if we have to play multichannel content
    uint32_t numEffects = 0;
    int ret = EffectQueryNumberEffects(&numEffects);
    if (ret != 0) {
        ALOGE("AudioMTKMixer() error %d querying number of effects", ret);
        return;
    }
    ALOGV("EffectQueryNumberEffects() numEffects=%d", numEffects);

    for (uint32_t i = 0 ; i < numEffects ; i++) {
        if (EffectQueryEffect(i, &dwnmFxDesc) == 0) {
            ALOGV("effect %d is called %s", i, dwnmFxDesc.name);
            if (memcmp(&dwnmFxDesc.type, EFFECT_UIID_DOWNMIX, sizeof(effect_uuid_t)) == 0) {
                ALOGI("found effect \"%s\" from %s",
                        dwnmFxDesc.name, dwnmFxDesc.implementor);
                isMultichannelCapable = true;
                break;
            }
        }
    }
    ALOGE_IF(!isMultichannelCapable, "unable to find downmix effect");
}

AudioMTKMixer::~AudioMTKMixer()
{
ALOGD("~AudioMTKMixer+");

    track_t* t = mState.tracks;
    for (unsigned i=0 ; i < MAX_NUM_TRACKS ; i++) {
        delete t->resampler;
        delete t->downmixerBufferProvider;
#ifdef TIME_STRETCH_ENABLE
        if(t->timestretchBufferProvider !=NULL)
        delete t->timestretchBufferProvider ;
#endif
#ifdef MTK_AUDIOMIXER_ENABLE_DRC
        if (t->mpDRCObj && t->mDRCState) {
            t->mpDRCObj->Close();
            t->mDRCState = false;
        }
        delete t->mpDRCObj;
#endif
#ifdef MTK_BESSURROUND_ENABLE
         delete  t->mSurroundMixer;
#endif
        t++;
    }
    delete [] mState.outputTemp;
    delete [] mState.resampleTemp;
    delete [] mState.aggregationBuffer;
#ifdef MTK_AUDIOMIXER_ENABLE_DRC
    delete [] mState.pDRCTempBuffer;
#endif

#ifdef MTK_AUDIOMIXER_ENABLE_LIMITER
    delete [] mState.pLimiterInternalBuffer;
    delete [] mState.pLimiterTempBuffer;
#endif
#ifdef MTK_BESSURROUND_ENABLE
delete [] mState.downMixBuffer;

#endif
ALOGD("~AudioMTKMixer-");
}

void AudioMTKMixer::setLog(NBLog::Writer *log)
{
    mState.mLog = log;
}

int AudioMTKMixer::getTrackName(audio_channel_mask_t channelMask, audio_format_t format, int sessionId)
{
    uint32_t names = (~mTrackNames) & mConfiguredNames;
    if (names != 0) {
        int n = __builtin_ctz(names);
        ALOGV("add track (%d)", n);
        mTrackNames |= 1 << n;
        // assume default parameters for the track, except where noted below
        track_t* t = &mState.tracks[n];
        t->needs = 0;
        t->volume[0] = 0;  //UNITY_GAIN_INT;
        t->volume[1] = 0;  //UNITY_GAIN_INT;
        // no initialization needed
        // t->prevVolume[0]
        // t->prevVolume[1]
        t->volumeInc[0] = 0;
        t->volumeInc[1] = 0;
        t->auxLevel = 0;
        t->auxInc = 0;
        // no initialization needed
        // t->prevAuxLevel
        // t->frameCount
        t->channelCount = 2;
        t->enabled = false;
        t->format = 16;
        t->mBitFormat = format;
        t->channelMask = AUDIO_CHANNEL_OUT_STEREO;
        t->sessionId = sessionId;
        // setBufferProvider(name, AudioBufferProvider *) is required before enable(name)
        t->bufferProvider = NULL;
        t->downmixerBufferProvider = NULL;
        t->buffer.raw = NULL;
        // no initialization needed
        // t->buffer.frameCount
        t->hook = NULL;
        t->in = NULL;
        t->resampler = NULL;
        t->sampleRate = mSampleRate;
        // setParameter(name, TRACK, MAIN_BUFFER, mixBuffer) is required before enable(name)
        t->mainBuffer = NULL;
        t->auxBuffer = NULL;
        // see t->localTimeFreq in constructor above
#ifdef MTK_BESSURROUND_ENABLE
                t->mSurroundMixer = NULL;
            t->mDevSampleRate = mSampleRate;
#endif

        status_t status = initTrackDownmix(&mState.tracks[n], n, channelMask);
        if (status == OK) {
            SetDRCCallback(this);
            return TRACK0 + n;
        }
        ALOGE("AudioMTKMixer::getTrackName(0x%x) failed, error preparing track for downmix",
                channelMask);
    }
    return -1;
}

void AudioMTKMixer::invalidateState(uint32_t mask)
{
    if (mask) {
        mState.needsChanged |= mask;
        mState.hook = process__validate;
    }
 }
#ifdef TIME_STRETCH_ENABLE
status_t AudioMTKMixer::initTrackTimeStretch(track_t* pTrack, int trackNum,  int ratio)
	{
		status_t status = OK;
		if (ratio> 55) {
			//pTrack->channelMask = mask;
			//pTrack->channelCount = channelCount;
	            if(pTrack->timestretchBufferProvider == NULL){
				ALOGD("initTrackTimeStretch(track=%d, ratio= %d) calls prepareTrackForTimeStretch()",
						trackNum, ratio);
				status = prepareTrackForTimeStretch(pTrack, trackNum,mState.frameCount,ratio);}
	            else{
			//ALOGD("initTrackTimeStretch(track=%d, ratio= %d) calls TimeStretchConfig()",
			//		trackNum, ratio);
	                  status =   pTrack->timestretchBufferProvider->TimeStretchConfig(ratio);
	                }
		} else {
			ALOGD("initTrackTimeStretch(track=%d, ratio= %d) calls unprepareTrackForTimeStretch()",
					trackNum, ratio);
			unprepareTrackForTimeStretch(pTrack, trackNum);
		}
		return status;
	}


void AudioMTKMixer::unprepareTrackForTimeStretch(track_t* pTrack, int trackName) {
    //ALOGV("AudioMTKMixer::unprepareTrackForTimeStretch(%d)", trackName);

    if (pTrack->timestretchBufferProvider != NULL) {
        // this track had previously been configured with a Time stretch, delete it
	if(pTrack->timestretchBufferProvider->mOutRemain !=0)
	{
		if(pTrack->resampler!= NULL)
		{
			// in case resampler keeps time stretched data.
		 	pTrack->resampler->ResetBuffer();
		 }
	}
        pTrack->bufferProvider = pTrack->timestretchBufferProvider->mTrackBufferProvider;
        delete pTrack->timestretchBufferProvider;
        pTrack->timestretchBufferProvider = NULL;
    } else {
        //ALOGV(" nothing to do, no timestretch to delete");
    }
}

status_t AudioMTKMixer::prepareTrackForTimeStretch(track_t* pTrack, int trackName, int framecount, int ratio)
{
    ALOGV("AudioMTKMixer::prepareTrackForTimeStretch(%d) with ratio 0x%x, framecount %d", trackName, ratio,framecount);

    // discard the previous downmixer if there was one
    unprepareTrackForTimeStretch(pTrack, trackName);

    int32_t status;

    if (!isTimeStretchCapable) {
        ALOGE("prepareTrackForTimeStretch(%d) fails: mixer doesn't support TimeStretch ",
                trackName);
        return NO_INIT;
    }
    if(pTrack->bufferProvider ==NULL)
    {
        ALOGE("prepareTrackForTimeStretch(%d) fails: pTrack->bufferProvider is null, pTrack 0x%x", trackName, pTrack);
        return NO_INIT;
    }
    TimeStretchBufferProvider* pDbp = new TimeStretchBufferProvider(framecount);
   if(pDbp == NULL)
   {
   	ALOGE("prepareTrackForTimeStretch(%d) fails: TimeStretchBufferProvider is null", trackName);
        return NO_INIT;
   }
    /*if(pTrack->mBitFormat != AUDIO_FORMAT_PCM_16_BIT)
    {
    
	ALOGE("prepareTrackForTimeStretch(%d) fails: TimeStretch doesn't support format other than AUDIO_FORMAT_PCM_16_BIT ",
			trackName);
	goto noTimeStretchForActiveTrack;
    }*/

    // channel input configuration will be overridden per-track
    pDbp->mTimeStretchConfig.inputCfg.channels =pTrack->channelMask;
    pDbp->mTimeStretchConfig.outputCfg.channels = AUDIO_CHANNEL_OUT_STEREO;
#ifdef MTK_HD_AUDIO_ARCHITECTURE
    pDbp->mTimeStretchConfig.inputCfg.format = pTrack->mBitFormat;
    pDbp->mTimeStretchConfig.outputCfg.format = pTrack->mBitFormat;
#else
    pDbp->mTimeStretchConfig.inputCfg.format = AUDIO_FORMAT_PCM_16_BIT;
    pDbp->mTimeStretchConfig.outputCfg.format = AUDIO_FORMAT_PCM_16_BIT;
#endif
    pDbp->mTimeStretchConfig.inputCfg.samplingRate = pTrack->sampleRate;
    pDbp->mTimeStretchConfig.outputCfg.samplingRate = pTrack->sampleRate;
    pDbp->mTimeStretchConfig.inputCfg.accessMode = EFFECT_BUFFER_ACCESS_READ;
    pDbp->mTimeStretchConfig.outputCfg.accessMode = EFFECT_BUFFER_ACCESS_WRITE;
    // input and output buffer provider, and frame count will not be used as the downmix effect
    // process() function is called directly (see DownmixerBufferProvider::getNextBuffer())
    pDbp->mTimeStretchConfig.inputCfg.mask = EFFECT_CONFIG_SMP_RATE | EFFECT_CONFIG_CHANNELS |
            EFFECT_CONFIG_FORMAT | EFFECT_CONFIG_ACC_MODE;
    pDbp->mTimeStretchConfig.outputCfg.mask = pDbp->mTimeStretchConfig.inputCfg.mask;

	// Configure and enable TimeStretch
     if(pDbp->mTimeStretchHandle->init(pTrack->sampleRate,  popcount(pTrack->channelMask), ratio) != 0)
     {
        ALOGE("prepareTrackForTimeStretch(%d) fails: Open Time stretch fail ",trackName);
        goto noTimeStretchForActiveTrack;
     	}
    // initialization successful:
    // - keep track of the real buffer provider in case it was set before
    pDbp->mTrackBufferProvider = pTrack->bufferProvider;
    // - we'll use the downmix effect integrated inside this
    //    track's buffer provider, and we'll use it as the track's buffer provider
    pTrack->timestretchBufferProvider = pDbp;
    pTrack->bufferProvider = pDbp;
    ALOGD("prepareTrackForTimeStretch, pTrack->bufferProvider : %x  pTrack->mTrackBufferProvider %x ",pTrack->bufferProvider,pDbp->mTrackBufferProvider );
    return NO_ERROR;

noTimeStretchForActiveTrack:
    delete pDbp;
    pTrack->timestretchBufferProvider = NULL;
    return NO_INIT;
}
#endif

status_t AudioMTKMixer::initTrackDownmix(track_t* pTrack, int trackNum, audio_channel_mask_t mask)
{
    uint32_t channelCount = popcount(mask);
    ALOG_ASSERT((channelCount <= MAX_NUM_CHANNELS_TO_DOWNMIX) && channelCount);
    status_t status = OK;
    if (channelCount > MAX_NUM_CHANNELS) {
        pTrack->channelMask = mask;
        pTrack->channelCount = channelCount;
        ALOGV("initTrackDownmix(track=%d, mask=0x%x) calls prepareTrackForDownmix()",
                trackNum, mask);
#ifndef MTK_BESSURROUND_ENABLE
        status = prepareTrackForDownmix(pTrack, trackNum);
#else
	status = prepareTrackForSurroundMix(pTrack, trackNum);
#endif
    } else {
        unprepareTrackForDownmix(pTrack, trackNum);
    }
    return status;
}

#ifdef MTK_BESSURROUND_ENABLE
status_t AudioMTKMixer::prepareTrackForSurroundMix(track_t* pTrack, int trackName){
	status_t ret;
		unprepareTrackForSurroundMix(pTrack, trackName);
    // init either normal downmix or surround down mix
	pTrack->mSurroundMixer = new AudioMTKSurroundDownMix();
         pTrack->mSurroundMixer->SetBesSurroundOnOFF(pTrack->mSurroundEnable);
         pTrack->mSurroundMixer->SetBesSurroundMode(pTrack->mSurroundMode);
	if (0 != (pTrack->mSurroundMixer->Init(trackName, pTrack->sessionId, pTrack->channelMask, pTrack->mDevSampleRate)))
        {   
	    return NO_INIT;
       }
	return  OK;
}

void AudioMTKMixer::unprepareTrackForSurroundMix(track_t* pTrack, int trackName) {
    if (pTrack->mSurroundMixer != NULL) {
        // this track had previously been configured with a downmixer, delete it
        delete pTrack->mSurroundMixer;
        pTrack->mSurroundMixer = NULL;
    } else  {
        ALOGV(" nothing to do, no downmixer to delete");
    }
}
#endif
void AudioMTKMixer::unprepareTrackForDownmix(track_t* pTrack, int trackName) {
    ALOGV("AudioMTKMixer::unprepareTrackForDownmix(%d)", trackName);
#ifdef MTK_BESSURROUND_SUPPORT
	unprepareTrackForSurroundMix(pTrack, trackName);	//override this function
#endif
    if (pTrack->downmixerBufferProvider != NULL) {
        // this track had previously been configured with a downmixer, delete it
        ALOGV(" deleting old downmixer");
        pTrack->bufferProvider = pTrack->downmixerBufferProvider->mTrackBufferProvider;
        delete pTrack->downmixerBufferProvider;
        pTrack->downmixerBufferProvider = NULL;
    } else {
        ALOGV(" nothing to do, no downmixer to delete");
    }
}

status_t AudioMTKMixer::prepareTrackForDownmix(track_t* pTrack, int trackName)
{
    ALOGV("AudioMTKMixer::prepareTrackForDownmix(%d) with mask 0x%x", trackName, pTrack->channelMask);

    // discard the previous downmixer if there was one
    unprepareTrackForDownmix(pTrack, trackName);

    DownmixerBufferProvider* pDbp = new DownmixerBufferProvider();
    int32_t status;

    if (!isMultichannelCapable) {
        ALOGE("prepareTrackForDownmix(%d) fails: mixer doesn't support multichannel content",
                trackName);
        goto noDownmixForActiveTrack;
    }

    if (EffectCreate(&dwnmFxDesc.uuid,
            pTrack->sessionId /*sessionId*/, -2 /*ioId not relevant here, using random value*/,
            &pDbp->mDownmixHandle/*pHandle*/) != 0) {
        ALOGE("prepareTrackForDownmix(%d) fails: error creating downmixer effect", trackName);
        goto noDownmixForActiveTrack;
    }

    // channel input configuration will be overridden per-track
    pDbp->mDownmixConfig.inputCfg.channels = pTrack->channelMask;
    pDbp->mDownmixConfig.outputCfg.channels = AUDIO_CHANNEL_OUT_STEREO;
#ifdef MTK_HD_AUDIO_ARCHITECTURE
    pDbp->mDownmixConfig.inputCfg.format = pTrack->mBitFormat;
    pDbp->mDownmixConfig.outputCfg.format = pTrack->mBitFormat;
#else
    pDbp->mDownmixConfig.inputCfg.format = AUDIO_FORMAT_PCM_16_BIT;
    pDbp->mDownmixConfig.outputCfg.format = AUDIO_FORMAT_PCM_16_BIT;
#endif
    pDbp->mDownmixConfig.inputCfg.samplingRate = pTrack->sampleRate;
    pDbp->mDownmixConfig.outputCfg.samplingRate = pTrack->sampleRate;
    pDbp->mDownmixConfig.inputCfg.accessMode = EFFECT_BUFFER_ACCESS_READ;
    pDbp->mDownmixConfig.outputCfg.accessMode = EFFECT_BUFFER_ACCESS_WRITE;
    // input and output buffer provider, and frame count will not be used as the downmix effect
    // process() function is called directly (see DownmixerBufferProvider::getNextBuffer())
    pDbp->mDownmixConfig.inputCfg.mask = EFFECT_CONFIG_SMP_RATE | EFFECT_CONFIG_CHANNELS |
            EFFECT_CONFIG_FORMAT | EFFECT_CONFIG_ACC_MODE;
    pDbp->mDownmixConfig.outputCfg.mask = pDbp->mDownmixConfig.inputCfg.mask;

    {// scope for local variables that are not used in goto label "noDownmixForActiveTrack"
        int cmdStatus;
        uint32_t replySize = sizeof(int);

        // Configure and enable downmixer
        status = (*pDbp->mDownmixHandle)->command(pDbp->mDownmixHandle,
                EFFECT_CMD_SET_CONFIG /*cmdCode*/, sizeof(effect_config_t) /*cmdSize*/,
                &pDbp->mDownmixConfig /*pCmdData*/,
                &replySize /*replySize*/, &cmdStatus /*pReplyData*/);
        if ((status != 0) || (cmdStatus != 0)) {
            ALOGE("error %d while configuring downmixer for track %d", status, trackName);
            goto noDownmixForActiveTrack;
        }
        replySize = sizeof(int);
        status = (*pDbp->mDownmixHandle)->command(pDbp->mDownmixHandle,
                EFFECT_CMD_ENABLE /*cmdCode*/, 0 /*cmdSize*/, NULL /*pCmdData*/,
                &replySize /*replySize*/, &cmdStatus /*pReplyData*/);
        if ((status != 0) || (cmdStatus != 0)) {
            ALOGE("error %d while enabling downmixer for track %d", status, trackName);
            goto noDownmixForActiveTrack;
        }

        // Set downmix type
        // parameter size rounded for padding on 32bit boundary
        const int psizePadded = ((sizeof(downmix_params_t) - 1)/sizeof(int) + 1) * sizeof(int);
        const int downmixParamSize =
                sizeof(effect_param_t) + psizePadded + sizeof(downmix_type_t);
        effect_param_t * const param = (effect_param_t *) malloc(downmixParamSize);
        param->psize = sizeof(downmix_params_t);
        const downmix_params_t downmixParam = DOWNMIX_PARAM_TYPE;
        memcpy(param->data, &downmixParam, param->psize);
        const downmix_type_t downmixType = DOWNMIX_TYPE_FOLD;
        param->vsize = sizeof(downmix_type_t);
        memcpy(param->data + psizePadded, &downmixType, param->vsize);

        status = (*pDbp->mDownmixHandle)->command(pDbp->mDownmixHandle,
                EFFECT_CMD_SET_PARAM /* cmdCode */, downmixParamSize/* cmdSize */,
                param /*pCmndData*/, &replySize /*replySize*/, &cmdStatus /*pReplyData*/);

        free(param);

        if ((status != 0) || (cmdStatus != 0)) {
            ALOGE("error %d while setting downmix type for track %d", status, trackName);
            goto noDownmixForActiveTrack;
        } else {
            ALOGV("downmix type set to %d for track %d", (int) downmixType, trackName);
        }
    }// end of scope for local variables that are not used in goto label "noDownmixForActiveTrack"

    // initialization successful:
    // - keep track of the real buffer provider in case it was set before
    pDbp->mTrackBufferProvider = pTrack->bufferProvider;
    // - we'll use the downmix effect integrated inside this
    //    track's buffer provider, and we'll use it as the track's buffer provider
    pTrack->downmixerBufferProvider = pDbp;
    pTrack->bufferProvider = pDbp;

    return NO_ERROR;

noDownmixForActiveTrack:
    delete pDbp;
    pDbp = NULL;
    pTrack->downmixerBufferProvider = NULL;
    return NO_INIT;
}

void AudioMTKMixer::deleteTrackName(int name)
{
    ALOGV("AudioMTKMixer::deleteTrackName(%d)", name);
    name -= TRACK0;
    ALOG_ASSERT(uint32_t(name) < MAX_NUM_TRACKS, "bad track name %d", name);
    ALOGV("deleteTrackName(%d)", name);
    track_t& track(mState.tracks[ name ]);
    if (track.enabled) {
        track.enabled = false;
        invalidateState(1<<name);
    }
    // delete the resampler
    if(NULL != track.resampler) {
        delete track.resampler;
        track.resampler = NULL;
    }
    // delete the downmixer
    unprepareTrackForDownmix(&mState.tracks[name], name);
    #ifdef TIME_STRETCH_ENABLE
    unprepareTrackForTimeStretch(&mState.tracks[name], name) ;
    #endif

#ifdef MTK_AUDIOMIXER_ENABLE_DRC
    track.mDRCEnable = false;
    if (track.mpDRCObj) {
        track.mpDRCObj->Close();
        track.mDRCState = false;
        if(NULL != track.mpDRCObj) {
            delete track.mpDRCObj;
            track.mpDRCObj = NULL;
        }
    }
#endif

    mTrackNames &= ~(1<<name);
}

void AudioMTKMixer::enable(int name)
{
    name -= TRACK0;
    ALOG_ASSERT(uint32_t(name) < MAX_NUM_TRACKS, "bad track name %d", name);
    track_t& track = mState.tracks[name];

    if (!track.enabled) {
        track.enabled = true;
        ALOGV("enable(%d)", name);
        invalidateState(1 << name);
        
    }
}

void AudioMTKMixer::disable(int name)
{
    name -= TRACK0;
    ALOG_ASSERT(uint32_t(name) < MAX_NUM_TRACKS, "bad track name %d", name);
    track_t& track = mState.tracks[name];

    if (track.enabled) {
        track.enabled = false;
        ALOGV("disable(%d)", name);
        invalidateState(1 << name);

    }
}

/* Sets the volume ramp variables for the AudioMixer.
 *
 * The volume ramp variables are used to transition from the previous
 * volume to the set volume.  ramp controls the duration of the transition.
 * Its value is typically one state framecount period, but may also be 0,
 * meaning "immediate."
 *
 * FIXME: 1) Volume ramp is enabled only if there is a nonzero integer increment
 * even if there is a nonzero floating point increment (in that case, the volume
 * change is immediate).  This restriction should be changed when the legacy mixer
 * is removed (see #2).
 * FIXME: 2) Integer volume variables are used for Legacy mixing and should be removed
 * when no longer needed.
 *
 * @param newVolume set volume target in floating point [0.0, 1.0].
 * @param ramp number of frames to increment over. if ramp is 0, the volume
 * should be set immediately.  Currently ramp should not exceed 65535 (frames).
 * @param pIntSetVolume pointer to the U4.12 integer target volume, set on return.
 * @param pIntPrevVolume pointer to the U4.28 integer previous volume, set on return.
 * @param pIntVolumeInc pointer to the U4.28 increment per output audio frame, set on return.
 * @param pSetVolume pointer to the float target volume, set on return.
 * @param pPrevVolume pointer to the float previous volume, set on return.
 * @param pVolumeInc pointer to the float increment per output audio frame, set on return.
 * @return true if the volume has changed, false if volume is same.
 */
static inline bool setVolumeRampVariables(float newVolume, int32_t ramp,
        int16_t *pIntSetVolume, int32_t *pIntPrevVolume, int32_t *pIntVolumeInc,
        float *pSetVolume, float *pPrevVolume, float *pVolumeInc) {
    if (newVolume == *pSetVolume) {
        return false;
    }
    /* set the floating point volume variables */
    if (ramp != 0) {
        *pVolumeInc = (newVolume - *pSetVolume) / ramp;
        *pPrevVolume = *pSetVolume;
    } else {
        *pVolumeInc = 0;
        *pPrevVolume = newVolume;
    }
    *pSetVolume = newVolume;

    /* set the legacy integer volume variables */
    int32_t intVolume = newVolume * AudioMTKMixer::UNITY_GAIN_INT;
    if (intVolume > AudioMTKMixer::UNITY_GAIN_INT) {
        intVolume = AudioMTKMixer::UNITY_GAIN_INT;
    } else if (intVolume < 0) {
        ALOGE("negative volume %.7g", newVolume);
        intVolume = 0; // should never happen, but for safety check.
    }
    if (intVolume == *pIntSetVolume) {
        *pIntVolumeInc = 0;
        /* TODO: integer/float workaround: ignore floating volume ramp */
        *pVolumeInc = 0;
        *pPrevVolume = newVolume;
        return true;
    }
    if (ramp != 0) {
        *pIntVolumeInc = ((intVolume - *pIntSetVolume) << 16) / ramp;
        *pIntPrevVolume = (*pIntVolumeInc == 0 ? intVolume : *pIntSetVolume) << 16;
    } else {
        *pIntVolumeInc = 0;
        *pIntPrevVolume = intVolume << 16;
    }
    *pIntSetVolume = intVolume;
    return true;
}

void AudioMTKMixer::setParameter(int name, int target, int param, void *value)
{
    name -= TRACK0;
    ALOG_ASSERT(uint32_t(name) < MAX_NUM_TRACKS, "bad track name %d", name);
    track_t& track = mState.tracks[name];

    int valueInt = (int)value;
    int32_t *valueBuf = (int32_t *)value;
    MTK_ALOG_V("AudioMTKMixer::setParameter name %d, target 0x%x,param 0x%x",name,target,param);
    switch (target) {

    case TRACK:
        switch (param) {
        case CHANNEL_MASK: {
            audio_channel_mask_t mask = (audio_channel_mask_t) value;
            if (track.channelMask != mask) {
                uint32_t channelCount = popcount(mask);
                ALOG_ASSERT((channelCount <= MAX_NUM_CHANNELS_TO_DOWNMIX) && channelCount);
                track.channelMask = mask;
                track.channelCount = channelCount;
                // the mask has changed, does this track need a downmixer?
                initTrackDownmix(&mState.tracks[name], name, mask);
                ALOGV("setParameter(TRACK, CHANNEL_MASK, %x)", mask);
                invalidateState(1 << name);
            }
            } break;
        case MAIN_BUFFER:
            if (track.mainBuffer != valueBuf) {
                track.mainBuffer = valueBuf;
                ALOGV("setParameter(TRACK, MAIN_BUFFER, %p)", valueBuf);
                invalidateState(1 << name);
            }
            break;
        case AUX_BUFFER:
            if (track.auxBuffer != valueBuf) {
                track.auxBuffer = valueBuf;
                ALOGV("setParameter(TRACK, AUX_BUFFER, %p)", valueBuf);
                invalidateState(1 << name);
            }
            break;
        case FORMAT:
#ifdef MTK_HD_AUDIO_ARCHITECTURE
            track.mBitFormat = (audio_format_t)valueInt;
            if (track.mBitFormat == AUDIO_FORMAT_PCM_16_BIT) {
                track.format = 16;
            } else if (track.mBitFormat == AUDIO_FORMAT_PCM_8_24_BIT) {
                track.format = 24;
            } else if (track.mBitFormat == AUDIO_FORMAT_PCM_32_BIT) {
                track.format = 32;
            }
#else
            ALOG_ASSERT(valueInt == AUDIO_FORMAT_PCM_16_BIT);
#endif
            break;
        // FIXME do we want to support setting the downmix type from AudioFlinger?
        //         for a specific track? or per mixer?
        /* case DOWNMIX_TYPE:
            break          */

#ifdef MTK_AUDIOMIXER_ENABLE_DRC
        case STREAM_TYPE:
            track.mStreamType = (audio_stream_type_t)valueInt;
            break;
#endif
        case DO_POST_PROC:
            track.IsMixerDoPostProc = (uint8_t) valueInt;
            MTK_ALOG_D("DC_REMOVE %d", track.IsMixerDoPostProc);
            break;
#ifdef TIME_STRETCH_ENABLE
        case DO_TIMESTRETCH:
            initTrackTimeStretch(&mState.tracks[name], name,valueInt);
            //MTK_ALOG_D("DO_TIMESTRETCH track name %d ration %d",name, valueInt);
            break;
#endif
        default:
            LOG_FATAL("bad param");
        }
        break;

    case RESAMPLE:
        switch (param) {
        case SAMPLE_RATE:
            ALOG_ASSERT(valueInt > 0, "bad sample rate %d", valueInt);
            if (track.setResampler(uint32_t(valueInt), mSampleRate)) {
                ALOGV("setParameter(RESAMPLE, SAMPLE_RATE, %u)",
                        uint32_t(valueInt));
                invalidateState(1 << name);
            }
            break;
        case RESET:
            track.resetResampler();
            invalidateState(1 << name);
            break;
        case REMOVE:
            if(NULL != track.resampler) {
                delete track.resampler;
                track.resampler = NULL;
            }
            track.sampleRate = mSampleRate;
            invalidateState(1 << name);
            break;
        default:
            LOG_FATAL("bad param");
        }
        break;

    case RAMP_VOLUME:
    case VOLUME:
        switch (param) {
        case VOLUME0:
        case VOLUME1:
            #if 0
            if (track.volume[param-VOLUME0] != valueInt) {
                ALOGV("setParameter(VOLUME, VOLUME0/1: %04x)", valueInt);
                track.prevVolume[param-VOLUME0] = track.volume[param-VOLUME0] << 16;
                track.volume[param-VOLUME0] = valueInt;
                if (target == VOLUME) {
                    track.prevVolume[param-VOLUME0] = valueInt << 16;
                    track.volumeInc[param-VOLUME0] = 0;
                } else {
                    int32_t d = (valueInt<<16) - track.prevVolume[param-VOLUME0];
                    int32_t volInc = d / int32_t(mState.frameCount);
                    track.volumeInc[param-VOLUME0] = volInc;
                    if (volInc == 0) {
                        track.prevVolume[param-VOLUME0] = valueInt << 16;
                    }
                }
                invalidateState(1 << name);
            }
            #else
            
            if (setVolumeRampVariables(*reinterpret_cast<float*>(value),
                    target == RAMP_VOLUME ? mState.frameCount : 0,
                    &track.volume[param - VOLUME0], &track.prevVolume[param - VOLUME0],
                    &track.volumeInc[param - VOLUME0],
                    &track.mVolume[param - VOLUME0], &track.mPrevVolume[param - VOLUME0],
                    &track.mVolumeInc[param - VOLUME0])) {
                ALOGD("setParameter(%s, VOLUME%d: %04x)",
                        target == VOLUME ? "VOLUME" : "RAMP_VOLUME", param - VOLUME0,
                                track.volume[param - VOLUME0]);
                invalidateState(1 << name);
            }
            #endif
            break;
        case AUXLEVEL:
            #if 0
            //ALOG_ASSERT(0 <= valueInt && valueInt <= MAX_GAIN_INT, "bad aux level %d", valueInt);
            if (track.auxLevel != valueInt) {
                ALOGV("setParameter(VOLUME, AUXLEVEL: %04x)", valueInt);
                track.prevAuxLevel = track.auxLevel << 16;
                track.auxLevel = valueInt;
                if (target == VOLUME) {
                    track.prevAuxLevel = valueInt << 16;
                    track.auxInc = 0;
                } else {
                    int32_t d = (valueInt<<16) - track.prevAuxLevel;
                    int32_t volInc = d / int32_t(mState.frameCount);
                    track.auxInc = volInc;
                    if (volInc == 0) {
                        track.prevAuxLevel = valueInt << 16;
                    }
                }
                invalidateState(1 << name);
            }
            #else
            if (setVolumeRampVariables(*reinterpret_cast<float*>(value),
                    target == RAMP_VOLUME ? mState.frameCount : 0,
                    &track.auxLevel, &track.prevAuxLevel, &track.auxInc,
                    &track.mAuxLevel, &track.mPrevAuxLevel, &track.mAuxInc)) {
                ALOGV("setParameter(%s, AUXLEVEL: %04x)",
                        target == VOLUME ? "VOLUME" : "RAMP_VOLUME", track.auxLevel);
                invalidateState(1 << name);
            }
            #endif
            break;
        default:
            LOG_FATAL("bad param");
        }
        break;

#ifdef MTK_AUDIOMIXER_ENABLE_DRC
    case DRC:
        switch (param) {
        case DEVICE:
            track.setDRCHandler(valueInt, mState.frameCount * MAX_NUM_CHANNELS * 2, mSampleRate);
            break;
        case RESET:
            track.updateDRCParam();
            break;
        default:
            LOG_FATAL("bad param");
        }
        break;
#endif

#ifdef MTK_BESSURROUND_ENABLE
    case SURROUND:  
        switch (param) {
        case DEVICE:   
                    if( track.mSurroundMixer !=NULL)
                    {track.mSurroundMixer->SetBesSurroundDevice(valueInt);}
            break;
        case BESSURND_ENABLE:
                    track.mSurroundEnable = valueInt;
                    MTK_ALOG_V("BESSURND_ENABLE %d",valueInt);
                    if(track.mSurroundMixer == NULL && track.mSurroundEnable == true )
                    {
                       prepareTrackForSurroundMix(&mState.tracks[name], name);
                    }
                    if( track.mSurroundMixer !=NULL)
                   {     track.mSurroundMixer->SetBesSurroundOnOFF(valueInt);}
                    break;
        case BESSURND_MODE:
            track.mSurroundMode = valueInt;
            //MTK_ALOG_V("BESSURND_MODE%d", valueInt);
            if( track.mSurroundMixer !=NULL)
           {         track.mSurroundMixer->SetBesSurroundMode(valueInt);}
            break;
        case RESET:
            if( track.mSurroundMixer !=NULL)
             {       track.mSurroundMixer->ResetBesSurround();}
             break;        
        default:
            LOG_FATAL("bad param");
    	}
        break;
#endif
    default:
        LOG_FATAL("bad target");
    }
}

#ifdef MTK_AUDIOMIXER_ENABLE_DRC

void AudioMTKMixer::track_t::updateDRCParam(void)
{
    ALOGD("updateDRCParam");
    if (mpDRCObj) {
        mpDRCObj->ResetBuffer();
        mpDRCObj->Close();
                
        mpDRCObj->SetParameter(BLOUD_PAR_SET_SAMPLE_RATE, (void *)sampleRate);
        mpDRCObj->SetParameter(BLOUD_PAR_SET_CHANNEL_NUMBER, (void *)BLOUD_HD_STEREO);
        mpDRCObj->SetParameter(BLOUD_PAR_SET_PCM_FORMAT, (void *)BLOUD_IN_Q1P31_OUT_Q1P31);
//        mpDRCObj->SetParameter(BLOUD_PAR_SET_FILTER_TYPE, (void *)AUDIO_COMP_FLT_AUDIO);
        mpDRCObj->SetParameter(BLOUD_PAR_SET_USE_DEFAULT_PARAM, (void *)NULL);
        mpDRCObj->SetParameter(BLOUD_PAR_SET_WORK_MODE, (void *)AUDIO_CMP_FLT_LOUDNESS_LITE);
        mpDRCObj->SetParameter(BLOUD_PAR_SET_STEREO_TO_MONO_MODE, (void *)mSteroToMono);
        mpDRCObj->Open();
    }
}

void AudioMTKMixer::track_t::setDRCHandler(audio_devices_t device, uint32_t bufferSize, uint32_t sampleRate)
{
    ALOGD("setDRCHandler, mUIDRCEnable %d, mpDRCObj 0x%x, mStreamType %d, device %d, mDRCState %d, this 0x%x", mUIDRCEnable, mpDRCObj, mStreamType, device, mDRCState, this);
    if ( (true==mUIDRCEnable) &&
        (device & AUDIO_DEVICE_OUT_SPEAKER) && (mStreamType != AUDIO_STREAM_DTMF)) {
        if (mpDRCObj == NULL) {
            //ALOGD("new MtkAudioLoud");
#if defined(MTK_AUDIO_BLOUD_CUSTOMPARAMETER_V4)||defined(MTK_AUDIO_BLOUD_CUSTOMPARAMETER_V3)
            mpDRCObj = new MtkAudioLoud(AUDIO_COMP_FLT_AUDIO);
#else
            if (mStreamType == AUDIO_STREAM_RING)
                mpDRCObj = new MtkAudioLoud(AUDIO_COMP_FLT_DRC_FOR_RINGTONE);
            else
                mpDRCObj = new MtkAudioLoud(AUDIO_COMP_FLT_DRC_FOR_MUSIC);
#endif
            
            mpDRCObj->SetParameter(BLOUD_PAR_SET_SAMPLE_RATE, (void *)sampleRate);
            mpDRCObj->SetParameter(BLOUD_PAR_SET_CHANNEL_NUMBER, (void *)BLOUD_HD_STEREO);
            mpDRCObj->SetParameter(BLOUD_PAR_SET_PCM_FORMAT, (void *)BLOUD_IN_Q1P31_OUT_Q1P31);
//            mpDRCObj->SetParameter(BLOUD_PAR_SET_FILTER_TYPE, (void *)AUDIO_COMP_FLT_AUDIO);
            mpDRCObj->SetParameter(BLOUD_PAR_SET_USE_DEFAULT_PARAM, (void *)NULL);
            mpDRCObj->SetParameter(BLOUD_PAR_SET_WORK_MODE, (void *)AUDIO_CMP_FLT_LOUDNESS_LITE);
            mpDRCObj->SetParameter(BLOUD_PAR_SET_STEREO_TO_MONO_MODE, (void *)mSteroToMono);            
            mpDRCObj->Open();
            mDRCState = true;
            mDRCEnable = true;
        }
        else {
            if(false == mDRCEnable) {
                //ALOGD("Change2Normal, mDRCEnable %d", mDRCEnable);
                if(ACE_SUCCESS == mpDRCObj->Change2Normal())
                    mDRCEnable = true;
            }
        }
    } else {
        if( (true==mDRCState) && (mpDRCObj != NULL)) {
            if(true == mDRCEnable) {
                //ALOGD("Change2ByPass, mDRCEnable %d", mDRCEnable);
                if(ACE_SUCCESS == mpDRCObj->Change2ByPass())
                    mDRCEnable = false;
            }
        }
    }
}


void AudioMTKMixer::track_t::applyDRC(int32_t *ioBuffer, uint32_t frameCount, int32_t *tempBuffer)
{
    uint32_t inputSampleCount, outputSampleCount;
    
    if ((mpDRCObj == NULL) || (!mDRCState))
       return;
    
    inputSampleCount = outputSampleCount = frameCount * MAX_NUM_CHANNELS * sizeof(int32_t);
    
    mpDRCObj->Process((void *)ioBuffer, &inputSampleCount, (void *)tempBuffer, &outputSampleCount);

    memcpy(ioBuffer, tempBuffer, frameCount*MAX_NUM_CHANNELS*sizeof(int32_t));
}

#endif

bool AudioMTKMixer::track_t::setResampler(uint32_t value, uint32_t devSampleRate)
{
    MTK_ALOG_V("AudioMTKMixer::track_t::setResampler sampleRate %d,value %d,devSampleRate = %d,resampler=%p,this %p",sampleRate,value,devSampleRate,resampler,this);

    sampleRate = value;
    if (resampler == NULL) {
    ALOGV("creating resampler from track %d Hz to device %d Hz", value, devSampleRate);
        AudioResampler::src_quality quality;
        // force lowest quality level resampler if use case isn't music or video
        // FIXME this is flawed for dynamic sample rates, as we choose the resampler
        // quality level based on the initial ratio, but that could change later.
        // Should have a way to distinguish tracks with static ratios vs. dynamic ratios.
        if (!((value == 44100 && devSampleRate == 48000) ||
              (value == 48000 && devSampleRate == 44100))) {
            quality = AudioResampler::LOW_QUALITY;
        } else {
            quality = AudioResampler::DEFAULT_QUALITY;
        }

        //mtk samplerate range must in (0.02, 25);
         int dstSampleRate= devSampleRate;
         int srcSampleRate = value;
        if(((dstSampleRate - srcSampleRate*50) < 0) && ((dstSampleRate*25 - srcSampleRate)>0))
        {
            quality = AudioResampler::MTK_QUALITY_32BIT;
        }
        resampler = AudioResampler::create(
                format,
                // the resampler sees the number of channels after the downmixer, if any
                downmixerBufferProvider != NULL ? MAX_NUM_CHANNELS : channelCount,
                devSampleRate,quality, srcSampleRate);
        resampler->setLocalTimeFreq(localTimeFreq);
    }
    return true;
}

inline
void AudioMTKMixer::track_t::adjustVolumeRamp(bool aux)
{
    for (uint32_t i=0 ; i<MAX_NUM_CHANNELS ; i++) {
        if (((volumeInc[i]>0) && (((prevVolume[i]+volumeInc[i])>>16) >= volume[i])) ||
            ((volumeInc[i]<0) && (((prevVolume[i]+volumeInc[i])>>16) <= volume[i]))) {
            volumeInc[i] = 0;
            prevVolume[i] = volume[i]<<16;
        }
    }
    if (aux) {
        if (((auxInc>0) && (((prevAuxLevel+auxInc)>>16) >= auxLevel)) ||
            ((auxInc<0) && (((prevAuxLevel+auxInc)>>16) <= auxLevel))) {
            auxInc = 0;
            prevAuxLevel = auxLevel<<16;
        }
    }
}

size_t AudioMTKMixer::getUnreleasedFrames(int name) const
{
    name -= TRACK0;
    if (uint32_t(name) < MAX_NUM_TRACKS) {
        return mState.tracks[name].getUnreleasedFrames();
    }
    return 0;
}

void AudioMTKMixer::setBufferProvider(int name, AudioBufferProvider* bufferProvider)
{
    name -= TRACK0;
    ALOG_ASSERT(uint32_t(name) < MAX_NUM_TRACKS, "bad track name %d", name);

    if (mState.tracks[name].downmixerBufferProvider != NULL) {
        // update required?
        if (mState.tracks[name].downmixerBufferProvider->mTrackBufferProvider != bufferProvider) {
            ALOGV("AudioMTKMixer::setBufferProvider(%p) for downmix", bufferProvider);
            // setting the buffer provider for a track that gets downmixed consists in:
            //  1/ setting the buffer provider to the "downmix / buffer provider" wrapper
            //     so it's the one that gets called when the buffer provider is needed,
            mState.tracks[name].bufferProvider = mState.tracks[name].downmixerBufferProvider;
            //  2/ saving the buffer provider for the track so the wrapper can use it
            //     when it downmixes.
            mState.tracks[name].downmixerBufferProvider->mTrackBufferProvider = bufferProvider;
        }
    } 
    
#ifdef TIME_STRETCH_ENABLE
	else if(mState.tracks[name].timestretchBufferProvider != NULL){
        
        // update required?
        if (mState.tracks[name].timestretchBufferProvider->mTrackBufferProvider != bufferProvider) {
           // ALOGV("AudioMixer::setBufferProvider(%p) for time stertch", bufferProvider);
            // setting the buffer provider for a track that gets downmixed consists in:
            //  1/ setting the buffer provider to the "downmix / buffer provider" wrapper
            //     so it's the one that gets called when the buffer provider is needed,
            mState.tracks[name].bufferProvider = mState.tracks[name].timestretchBufferProvider;
            //  2/ saving the buffer provider for the track so the wrapper can use it
            //     when it downmixes.
            mState.tracks[name].timestretchBufferProvider->mTrackBufferProvider = bufferProvider;
        	}
    	}
#endif
    else {
        mState.tracks[name].bufferProvider = bufferProvider;
    }
}



void AudioMTKMixer::process(int64_t pts)
{
    mState.hook(&mState, pts);
}


void AudioMTKMixer::process__validate(state_t* state, int64_t pts)
{
    ALOGW_IF(!state->needsChanged,
        "in process__validate() but nothing's invalid");

    uint32_t changed = state->needsChanged;
    state->needsChanged = 0; // clear the validation flag

    // recompute which tracks are enabled / disabled
    uint32_t enabled = 0;
    uint32_t disabled = 0;
    while (changed) {
        const int i = 31 - __builtin_clz(changed);
        const uint32_t mask = 1<<i;
        changed &= ~mask;
        track_t& t = state->tracks[i];
        (t.enabled ? enabled : disabled) |= mask;
    }
    state->enabledTracks &= ~disabled;
    state->enabledTracks |=  enabled;

    // compute everything we need...
    int countActiveTracks = 0;
    bool all16BitsStereoNoResample = true;
    bool resampling = false;
    bool volumeRamp = false;
    uint32_t en = state->enabledTracks;
    while (en) {
        const int i = 31 - __builtin_clz(en);
        en &= ~(1<<i);

        countActiveTracks++;
        track_t& t = state->tracks[i];
        uint32_t n = 0;
        n |= NEEDS_CHANNEL_1 + t.channelCount - 1;
        n |= NEEDS_FORMAT_16;
        n |= NEEDS_RESAMPLE_ENABLED;
        if (t.auxLevel != 0 && t.auxBuffer != NULL) {
            n |= NEEDS_AUX_ENABLED;
        }

        if (t.volumeInc[0]|t.volumeInc[1]) {
            volumeRamp = true;
        }
        t.needs = n;

        {
            if ((n & NEEDS_AUX__MASK) == NEEDS_AUX_ENABLED) {
                all16BitsStereoNoResample = false;
            }
            if ((n & NEEDS_RESAMPLE__MASK) == NEEDS_RESAMPLE_ENABLED) {
                all16BitsStereoNoResample = false;
                resampling = true;
                t.hook = track__genericResample;
                ALOGV_IF((n & NEEDS_CHANNEL_COUNT__MASK) > NEEDS_CHANNEL_2,
                        "Track %d needs downmix + resample", i);
            }
        }
    }

    // select the processing hooks
    state->hook = process__nop;
    if (countActiveTracks) {
        state->hook = process__genericResampling;
    }

    ALOGV("mixer configuration change: %d activeTracks (%08x) "
        "all16BitsStereoNoResample=%d, resampling=%d, volumeRamp=%d",
        countActiveTracks, state->enabledTracks,
        all16BitsStereoNoResample, resampling, volumeRamp);

    state->hook(state, pts);
}

#ifdef MTK_BESSURROUND_ENABLE
void AudioMTKMixer::track__Downmix(track_t* t, int32_t* inBuffer, size_t numFrames,state_t* state)
{
    if(!t ->mSurroundMixer)
        {
            track__addToAggregationBuffer(state, inBuffer, numFrames);
            return ;
        }else{
            MTK_ALOG_V(" surroundMix process");
            t->mSurroundMixer->process(inBuffer,(state->downMixBuffer),numFrames);            
            track__addToAggregationBuffer(state, (state->downMixBuffer), numFrames);
            return ;
            }
}
#endif
void AudioMTKMixer::track__addToAggregationBuffer(state_t* state, int32_t* inBuffer, size_t numFrames)
{
   // Error case
   if (!state->aggregationBuffer)
      return;

   // *MAX_NUM_CHANNELS for stereo
   memcpy(state->aggregationBuffer + state->aggregationCount*MAX_NUM_CHANNELS, inBuffer, numFrames*sizeof(int32_t)*MAX_NUM_CHANNELS);
   state->aggregationCount += numFrames;
}

void AudioMTKMixer::track__addToAggregationBuffer_16(state_t* state, const int16_t* inBuffer, size_t numFrames, uint32_t channelNum)
{
   uint32_t i;
   int32_t *pDest = state->aggregationBuffer + state->aggregationCount*MAX_NUM_CHANNELS;
   
   // Error case
   if (!state->aggregationBuffer)
      return;
   
   if (channelNum == 1) {
      // Mono
      int16_t temp;
      
      for (i=0; i<numFrames; i++) {
         temp = (*inBuffer++) << 12;
         *pDest++ = temp;
         *pDest++ = temp;
      }
   } else {
      // Stereo
      for (i=0; i<numFrames; i++) {
         *pDest++ = (*inBuffer++) << 12;
         *pDest++ = (*inBuffer++) << 12;
      }
   }
   
   state->aggregationCount += numFrames;
}

void AudioMTKMixer::track__genericResample(track_t* t, size_t outFrameCount, int32_t* temp, int32_t* aux, state_t* state)
{
    t->resampler->setSampleRate(t->sampleRate);

    // ramp gain - resample to temp buffer and scale/mix in 2nd step
    if (aux != NULL) {
        // always resample with unity gain when sending to auxiliary buffer to be able
        // to apply send level after resampling
        // TODO: modify each resampler to support aux channel?
        t->resampler->setVolume(UNITY_GAIN_INT, UNITY_GAIN_INT);
#ifdef MTK_BESSURROUND_ENABLE
        memset(temp, 0, outFrameCount * MAX_NUM_CHANNELS_TO_DOWNMIX * sizeof(int32_t));
#else
        memset(temp, 0, outFrameCount * MAX_NUM_CHANNELS * sizeof(int32_t));
#endif
        t->resampler->resample(temp, outFrameCount, t->bufferProvider);
        #ifdef MTK_BESSURROUND_ENABLE
        track__Downmix( t, temp, outFrameCount,state);
        #else
        track__addToAggregationBuffer(state, temp, outFrameCount);
        #endif
    } else {
        if (CC_UNLIKELY(t->volumeInc[0]|t->volumeInc[1])) {
            t->resampler->setVolume(UNITY_GAIN_INT, UNITY_GAIN_INT);
#ifdef MTK_BESSURROUND_ENABLE
                    memset(temp, 0, outFrameCount * MAX_NUM_CHANNELS_TO_DOWNMIX * sizeof(int32_t));
#else
                    memset(temp, 0, outFrameCount * MAX_NUM_CHANNELS * sizeof(int32_t));
#endif
            t->resampler->resample(temp, outFrameCount, t->bufferProvider);
#ifdef MTK_BESSURROUND_ENABLE
            track__Downmix( t, temp, outFrameCount,state);
#else
            track__addToAggregationBuffer(state, temp, outFrameCount);
#endif
        }

        // constant gain
        else {
            t->resampler->setVolume(UNITY_GAIN_INT, UNITY_GAIN_INT);
#ifdef MTK_BESSURROUND_ENABLE
                    memset(temp, 0, outFrameCount * MAX_NUM_CHANNELS_TO_DOWNMIX * sizeof(int32_t));
#else
                    memset(temp, 0, outFrameCount * MAX_NUM_CHANNELS * sizeof(int32_t));
#endif
            t->resampler->resample(temp, outFrameCount, t->bufferProvider);
#ifdef MTK_BESSURROUND_ENABLE
                        track__Downmix( t, temp, outFrameCount,state);
#else
            track__addToAggregationBuffer(state, temp, outFrameCount);
#endif
        }
    }
}

void AudioMTKMixer::track__nop(track_t* t, size_t outFrameCount, int32_t* temp, int32_t* aux, state_t* state)
{
}

void AudioMTKMixer::volumeRampStereo(track_t* t, int64_t* out, size_t frameCount, int32_t* temp, int32_t* aux)
{
    int32_t vl = t->prevVolume[0];
    int32_t vr = t->prevVolume[1];
    const int32_t vlInc = t->volumeInc[0];
    const int32_t vrInc = t->volumeInc[1];

    //ALOGD("[0] %p: inc=%f, v0=%f, v1=%d, final=%f, count=%d",
    //        t, vlInc/65536.0f, vl/65536.0f, t->volume[0],
    //       (vl + vlInc*frameCount)/65536.0f, frameCount);

    // ramp volume
    if (CC_UNLIKELY(aux != NULL)) {
        int32_t va = t->prevAuxLevel;
        const int32_t vaInc = t->auxInc;
        int64_t l;
        int64_t r;
        int64_t tempAux;

        do {
            l = (*temp++);
            r = (*temp++);
            *out++ += (int64_t)(vl >> 16) * l;
            *out++ += (int64_t)(vr >> 16) * r;
            tempAux = (l + r)>>13; // 16 bits resolution
            tempAux *= (va >> 16);
            *aux++ += (int32_t)tempAux;
            vl += vlInc;
            vr += vrInc;
            va += vaInc;
        } while (--frameCount);
        t->prevAuxLevel = va;
    } else {
        do {
            *out++ += (int64_t)(vl >> 16) * (*temp++);
            *out++ += (int64_t)(vr >> 16) * (*temp++);
            vl += vlInc;
            vr += vrInc;
        } while (--frameCount);
    }
    t->prevVolume[0] = vl;
    t->prevVolume[1] = vr;
    t->adjustVolumeRamp(aux != NULL);
}

void AudioMTKMixer::volumeStereo(track_t* t, int64_t* out, size_t frameCount, int32_t* temp, int32_t* aux)
{
    const int16_t vl = t->volume[0];
    const int16_t vr = t->volume[1];
    ALOGD("[0] %p: v0=%f, v1=%d",
            t,  vl/65536.0f, t->volume[0]
          );


    if (CC_UNLIKELY(aux != NULL)) {
        const int16_t va = t->auxLevel;
        int64_t tempAux;
        do {
            int64_t l = (*temp++);
            int64_t r = (*temp++);
            out[0] += l * vl;
            out[1] += r * vr;
            out += 2;
            
            tempAux = (l + r) >> 13;
            aux[0] += (int32_t)tempAux * va;
            aux++;
        } while (--frameCount);
    } else {
        do {
            int64_t l = (*temp++);
            int64_t r = (*temp++);
            out[0] += l * vl;
            out[1] += r * vr;
            out += 2;
        } while (--frameCount);
    }
}

void AudioMTKMixer::genericVolumeRampStereo(state_t* state, track_t* t, int64_t* out)
{
    size_t frameCount = state->frameCount;
    int32_t* temp = state->aggregationBuffer;
    int32_t* aux = t->auxBuffer;
    
    if (aux != NULL) {
        if (CC_UNLIKELY(t->volumeInc[0]|t->volumeInc[1]|t->auxInc)) {
            volumeRampStereo(t, out, frameCount, temp, aux);
        } else {
            volumeStereo(t, out, frameCount, temp, aux);
        }
    } else {
        if (CC_UNLIKELY(t->volumeInc[0]|t->volumeInc[1])) {
            volumeRampStereo(t, out, frameCount, temp, aux);
        } else {
            volumeStereo(t, out, frameCount, temp, aux);
        }
    }
}

void AudioMTKMixer::process__resetAggregationBuffer(state_t* state)
{
   // Error case
   if (!state->aggregationBuffer)
      return;
   
   memset(state->aggregationBuffer, 0, state->frameCount * sizeof(int32_t) * MAX_NUM_CHANNELS);
   state->aggregationCount = 0;
}

// no-op case
void AudioMTKMixer::process__nop(state_t* state, int64_t pts)
{
    uint32_t e0 = state->enabledTracks;
    size_t bufSize = state->frameCount * sizeof(int16_t) * MAX_NUM_CHANNELS;
    while (e0) {
        // process by group of tracks with same output buffer to
        // avoid multiple memset() on same buffer
        uint32_t e1 = e0, e2 = e0;
        int i = 31 - __builtin_clz(e1);
        track_t& t1 = state->tracks[i];
        e2 &= ~(1<<i);
        while (e2) {
            i = 31 - __builtin_clz(e2);
            e2 &= ~(1<<i);
            track_t& t2 = state->tracks[i];
            if (CC_UNLIKELY(t2.mainBuffer != t1.mainBuffer)) {
                e1 &= ~(1<<i);
            }
        }
        e0 &= ~(e1);

        memset(t1.mainBuffer, 0, bufSize);

        while (e1) {
            i = 31 - __builtin_clz(e1);
            e1 &= ~(1<<i);
            t1 = state->tracks[i];
            size_t outFrames = state->frameCount;
            while (outFrames) {
                t1.buffer.frameCount = outFrames;
                int64_t outputPTS = calculateOutputPTS(
                    t1, pts, state->frameCount - outFrames);
                t1.bufferProvider->getNextBuffer(&t1.buffer, outputPTS);
                if (t1.buffer.raw == NULL) break;
                outFrames -= t1.buffer.frameCount;
                t1.bufferProvider->releaseBuffer(&t1.buffer);
            }
        }
    }
}

// generic code with resampling
void AudioMTKMixer::process__genericResampling(state_t* state, int64_t pts)
{
//ALOGD("AudioMTKMixer::process__genericResampling+");

    // this const just means that local variable outTemp doesn't change
    int64_t* const outTemp = state->outputTemp;
    const size_t size = sizeof(int64_t) * MAX_NUM_CHANNELS * state->frameCount;

    size_t numFrames = state->frameCount;

    uint32_t e0 = state->enabledTracks;
    while (e0) {
        // process by group of tracks with same output buffer
        // to optimize cache use
        uint32_t e1 = e0, e2 = e0;
        int j = 31 - __builtin_clz(e1);
        track_t& t1 = state->tracks[j];
        e2 &= ~(1<<j);
        while (e2) {
            j = 31 - __builtin_clz(e2);
            e2 &= ~(1<<j);
            track_t& t2 = state->tracks[j];
            if (CC_UNLIKELY(t2.mainBuffer != t1.mainBuffer)) {
                e1 &= ~(1<<j);
            }
        }
        e0 &= ~(e1);
        int32_t *out = t1.mainBuffer;
        memset(outTemp, 0, size);

        while (e1) {
            const int i = 31 - __builtin_clz(e1);
            e1 &= ~(1<<i);
            track_t& t = state->tracks[i];
            int32_t *aux = NULL;
            if (CC_UNLIKELY((t.needs & NEEDS_AUX__MASK) == NEEDS_AUX_ENABLED)) {
                aux = t.auxBuffer;
            }

            process__resetAggregationBuffer(state);

            // this is a little goofy, on the resampling case we don't
            // acquire/release the buffers because it's done by
            // the resampler.
            if ((t.needs & NEEDS_RESAMPLE__MASK) == NEEDS_RESAMPLE_ENABLED) {
                t.resampler->setPTS(pts);
                t.hook(&t, numFrames, state->resampleTemp, aux, state);
            }

#ifdef MTK_AUDIOMIXER_ENABLE_DRC
            if(t.IsMixerDoPostProc)
            {   
                MTK_ALOG_D("Do DRC and volume control");
                t.applyDRC( state->aggregationBuffer, state->frameCount, state->pDRCTempBuffer);
                MixerDumpPcm(gaf_mixer_drc_pcm, gaf_mixer_drc_propty, gettid(), (int)&t, state->aggregationBuffer,sizeof(int32_t) * state->frameCount * MAX_NUM_CHANNELS);
            }
#endif
            genericVolumeRampStereo(state, &t, outTemp);
        }

        MixerDumpPcm(gaf_mixer_gain_pcm, gaf_mixer_gain_propty, gettid(), 0, outTemp, sizeof(int64_t) * state->frameCount * MAX_NUM_CHANNELS);

#ifdef MTK_HD_AUDIO_ARCHITECTURE

#ifdef MTK_AUDIOMIXER_ENABLE_LIMITER
        {
            uint32_t inSize, outSize;
            uint32_t i;
            int64_t *pTemp64 = outTemp;
            int64_t tempValue;
            
            for (i=0; i < numFrames * MAX_NUM_CHANNELS; i++) {
                tempValue = *pTemp64;
                *pTemp64++ = tempValue >> 12;  // 12 bits gain
            }
            
            inSize = numFrames * MAX_NUM_CHANNELS * sizeof(int64_t);
            outSize = numFrames * MAX_NUM_CHANNELS * sizeof(int32_t);
            Limiter_Process( state->mpLimiterObj, (char *)state->pLimiterTempBuffer,
                             (void *)outTemp, &inSize,
                             (void *)out, &outSize );
        }
        MixerDumpPcm(gaf_mixer_limin_pcm, gaf_mixer_limin_propty, gettid(), 0, out, sizeof(int32_t) * state->frameCount * MAX_NUM_CHANNELS);
#else
        // TBD: use limiter SWIP
        // Transform 64 bits into 1.31 format
        {
            int64_t *pSrc64 = outTemp;
            int32_t *pDest32 = out;
            int64_t tempValue;
            uint32_t i;
            
            for (i=0; i<numFrames * MAX_NUM_CHANNELS; i++) {
                tempValue = (*pSrc64++);  // Shift 12 bits for unit gain
                if ((tempValue>>31) ^ (tempValue>>63)) {
                    tempValue = 0x7FFFFFFF ^ (tempValue>>63);
                }
                *pDest32++ = tempValue;
            }
        }
#endif
        // Test for 4.28 format
        //memcpy(out, outTemp, numFrames * MAX_NUM_CHANNELS * sizeof(int32_t));
#else
        // Original method. out is 16 bits
        ditherAndClamp(out, outTemp, numFrames);
#endif
    }

//ALOGD("AudioMTKMixer::process__genericResampling-");
}

int64_t AudioMTKMixer::calculateOutputPTS(const track_t& t, int64_t basePTS,
                                       int outputFrameIndex)
{
    if (AudioBufferProvider::kInvalidPTS == basePTS)
        return AudioBufferProvider::kInvalidPTS;

    return basePTS + ((outputFrameIndex * t.localTimeFreq) / t.sampleRate);
}

// ----------------------------------------------------------------------------
}; // namespace android

