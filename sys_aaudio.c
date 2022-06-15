/*
  Faun Android backend using AAudio (API level 26)
*/

#include <aaudio/AAudio.h>
#include <android/log.h>

static const char* sysaudio_open(const char* appName)
{
    (void) appName;
    return NULL;
}

static void sysaudio_close()
{
}

static const char* sysaudio_allocVoice(FaunVoice* voice, int updateHz,
                                       const char* appName)
{
    AAudioStream* stream;
    AAudioStreamBuilder* bld;
    aaudio_result_t res;
    int32_t abufferCap;
    int32_t abufferSize;
    int32_t framesPerBurst;
    int32_t mixLen;
    int mixRate = voice->mix.rate;
    int fmt;
    int chan = faun_channelCount(voice->mix.chanLayout);

    voice->backend = NULL;

    switch (voice->mix.format) {
        case FAUN_S16:
            fmt = AAUDIO_FORMAT_PCM_I16;
            voice->frameBytes = chan * sizeof(int16_t);
            break;
        case FAUN_F32:
            fmt = AAUDIO_FORMAT_PCM_FLOAT;
            voice->frameBytes = chan * sizeof(float);
            break;
        default:
            return "Invalid Faun sample format for AAudio";
    }

    res = AAudio_createStreamBuilder(&bld);
    if (res != AAUDIO_OK)
        return "Cannot create AAudio stream builder";

    // Use the default device & direction (output stream).
    AAudioStreamBuilder_setSharingMode(bld, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setSampleRate(bld, mixRate);
    AAudioStreamBuilder_setChannelCount(bld, chan);
    AAudioStreamBuilder_setFormat(bld, fmt);
    AAudioStreamBuilder_setBufferCapacityInFrames(bld, (mixRate/updateHz) * 2);
    /*
    AAudioStreamBuilder_setPerformanceMode(bld,
                                    AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    */

    res = AAudioStreamBuilder_openStream(bld, &stream);
    AAudioStreamBuilder_delete(bld);
    if (res != AAUDIO_OK)
        return "Cannot open AAudio stream";

    framesPerBurst = AAudioStream_getFramesPerBurst(stream);

    // Set voice->mix.used to one or two bursts.
    mixLen = framesPerBurst;
    if ((mixRate / framesPerBurst) > 66)
        mixLen += framesPerBurst;
    faun_reserve(&voice->mix, mixLen);
    voice->mix.used = mixLen;
    voice->updateHz = mixRate / mixLen;

    AAudioStream_setBufferSizeInFrames(stream, mixLen * 2);
    abufferSize = AAudioStream_getBufferSizeInFrames(stream);
    abufferCap  = AAudioStream_getBufferCapacityInFrames(stream);

#if 1
    __android_log_print(ANDROID_LOG_INFO, "faun",
                        "mix: %d hz: %d\n(capacity: %d size: %d burst: %d)\n",
                        voice->mix.used,
                        voice->updateHz,
                        abufferCap, abufferSize, framesPerBurst);
#endif

    res = AAudioStream_requestStart(stream);
    if (res != AAUDIO_OK) {
        AAudioStream_close(stream);
        return "Cannot start AAudio stream";
    }

    voice->backend = stream;
    return NULL;
}

static void sysaudio_freeVoice(FaunVoice *voice)
{
    AAudioStream_close((AAudioStream*) voice->backend);
    voice->backend = NULL;
}

static const char* sysaudio_write(FaunVoice* voice, const void* data,
                                  uint32_t len)
{
    AAudioStream* stream = (AAudioStream*) voice->backend;
    aaudio_result_t res;
    int32_t numFrames = len / voice->frameBytes;

    /*
    AAudio docs say:
      Audio latency is high for blocking write() because the Android O DP2
      release doesn't use a FAST track. Use a callback to get lower latency.
    */

    res = AAudioStream_write(stream, data, numFrames, 25000000);
    if (res < 0)
        return AAudio_convertResultToText(res);
#if 1
    if (res != numFrames)
        __android_log_print(ANDROID_LOG_WARN, "faun",
                            "underrun: %d\n",
                            AAudioStream_getXRunCount(stream));
#endif

    return NULL;
}

static int sysaudio_startVoice(FaunVoice *voice)
{
    AAudioStream* stream = (AAudioStream*) voice->backend;
    return (AAudioStream_requestStart(stream) == AAUDIO_OK);
}

static int sysaudio_stopVoice(FaunVoice *voice)
{
    AAudioStream* stream = (AAudioStream*) voice->backend;
    return (AAudioStream_requestPause(stream) == AAUDIO_OK);
}
