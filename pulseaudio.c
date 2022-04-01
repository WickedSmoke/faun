/*
  Faun PulseAudio backend
*/

#include <pulse/simple.h>
#include <pulse/error.h>

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
    // This table is synced with FaunSampleFormat.
    static const uint8_t _paFormat[FAUN_FORMAT_COUNT] = {
        PA_SAMPLE_U8,
        PA_SAMPLE_S16NE,
        PA_SAMPLE_S24NE,
        PA_SAMPLE_FLOAT32NE
    };
    pa_simple* ps;
    pa_sample_spec ss;
    pa_buffer_attr ba;
    int error;
    (void) updateHz;

    ss.channels = faun_channelCount(voice->mix.chanLayout);
    ss.rate     = voice->mix.rate;
    ss.format   = _paFormat[voice->mix.format];

    // Use default attributes except for a lower latency.
    // This can be overridden by the PULSE_LATENCY_MSEC environment variable.
    ba.maxlength = -1;
    ba.tlength   = pa_usec_to_bytes(50 * 1000, &ss);  // 50 ms of latency
    ba.prebuf    = -1;
    ba.minreq    = -1;
    ba.fragsize  = -1;

    // Use the default server, device, & channel map.
    ps = pa_simple_new(NULL, appName, PA_STREAM_PLAYBACK,
                       NULL, "Faun Voice", &ss, NULL, &ba, &error);
    if (! ps)
        return pa_strerror(error);

    voice->backend = ps;
    return NULL;
}

static void sysaudio_freeVoice(FaunVoice *voice)
{
    pa_simple_free((pa_simple*) voice->backend);
    voice->backend = NULL;
}

static const char* sysaudio_write(FaunVoice* voice, const void* data,
                                  uint32_t len)
{
    pa_simple* ps = (pa_simple*) voice->backend;
    int error;
    if (pa_simple_write(ps, data, len, &error) < 0)
        return pa_strerror(error);
    return NULL;
}

static int sysaudio_startVoice(FaunVoice *voice)
{
    (void) voice;
    return 1;
}

static int sysaudio_stopVoice(FaunVoice *voice)
{
    (void) voice;
    return 0;
}

/*
static int sysaudio_isPlaying(const FaunVoice *voice)
{
    (void) voice;
    return 1;
}
*/
