/*
  Faun PulseAudio backend
*/

#include <pulse/pulseaudio.h>

typedef struct {
    pa_mainloop* loop;
    pa_context*  context;
    pa_stream*   stream;
}
PulseSession;

static PulseSession paSession;

static void sysaudio_close()
{
    if (paSession.context) {
        pa_context_disconnect(paSession.context);
        pa_context_unref(paSession.context);
        paSession.context = NULL;
    }

    if (paSession.loop) {
        pa_mainloop_free(paSession.loop);
        paSession.loop = NULL;
    }
}

static const char* sysaudio_open(const char* appName)
{
    int error;

    paSession.stream  = NULL;
    paSession.loop    = pa_mainloop_new();
    paSession.context = pa_context_new(pa_mainloop_get_api(paSession.loop),
                                       appName);
    if (! paSession.context) {
        sysaudio_close();
        return "pa_context_new failed";
    }

    error = pa_context_connect(paSession.context, NULL, 0, NULL);
    if (error) {
        sysaudio_close();
        return "pa_context_connect failed";
    }

    return NULL;
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
    pa_sample_spec ss;
    pa_buffer_attr ba;
    int error;
    pa_usec_t dur = 2500000 / updateHz;
                //= 50 * 1000;      // 50 ms latency
    (void) appName;

    ss.channels = faun_channelCount(voice->mix.chanLayout);
    ss.rate     = voice->mix.rate;
    ss.format   = _paFormat[voice->mix.format];

    // Use default attributes except for a lower latency.
    // This can be overridden by the PULSE_LATENCY_MSEC environment variable.
    ba.maxlength = -1;
    ba.tlength   = pa_usec_to_bytes(dur, &ss);
    ba.prebuf    = -1;
    ba.minreq    = -1;
    ba.fragsize  = -1;

    while (! paSession.stream) {
        error = pa_mainloop_iterate(paSession.loop, 1, NULL);
        if (error < 0)
            return pa_strerror(error);

        switch (pa_context_get_state(paSession.context)) {
            case PA_CONTEXT_READY:
                paSession.stream = pa_stream_new(paSession.context,
                                                 "Faun Voice", &ss, NULL);
                if (! paSession.stream)
                    return "pa_stream_new failed";
                break;
            case PA_CONTEXT_FAILED:
                return "PA_CONTEXT_FAILED";
            case PA_CONTEXT_TERMINATED:
                return "PA_CONTEXT_TERMINATED";
            default:
                break;
        }
    }

    // Use the default device & volume.  Flags are the same as pa_simple_new.
    error = pa_stream_connect_playback(paSession.stream, NULL, &ba,
                                       PA_STREAM_INTERPOLATE_TIMING |
                                       PA_STREAM_ADJUST_LATENCY |
                                       PA_STREAM_AUTO_TIMING_UPDATE,
                                       NULL, NULL);
    if (error)
        return pa_strerror(error);

    for (;;) {
        pa_stream_state_t state = pa_stream_get_state(paSession.stream);
        if (state == PA_STREAM_READY) {
#if 0
            const pa_buffer_attr* sa =
                pa_stream_get_buffer_attr(paSession.stream);
            printf("KR buffer_attr %d %d %d %d\n",
                   sa->maxlength, sa->tlength, sa->prebuf, sa->minreq);
#endif
            break;
        }
        if (state != PA_STREAM_CREATING)
            return "pa_stream_connect_playback failed";

        error = pa_mainloop_iterate(paSession.loop, 1, NULL);
        if (error < 0)
            return pa_strerror(error);
    }

    voice->backend = &paSession;
    return NULL;
}

#define PS  ((PulseSession*) voice->backend)

static void sysaudio_freeVoice(FaunVoice *voice)
{
    PulseSession* ps = PS;
    if (ps) {
        pa_stream_disconnect(ps->stream);
        pa_stream_unref(ps->stream);

        voice->backend = NULL;
    }
}

static const char* sysaudio_write(FaunVoice* voice, const void* data,
                                  uint32_t len)
{
    PulseSession* ps = PS;
    size_t nr;
    int error;

    // Use pa_stream_writable_size to throttle the output, but feed all data
    // with a single write so that we can return ASAP.  The actual write
    // limit is buffer_attr.maxlength.

    while (! (nr = pa_stream_writable_size(ps->stream))) {
        //printf("KR nr .\n");
        error = pa_mainloop_iterate(ps->loop, 1, NULL);
        if (error < 0)
            return pa_strerror(error);
    }

    //printf("KR nr %ld\n", nr);
    pa_stream_write(ps->stream, data, len, NULL, 0, PA_SEEK_RELATIVE);
    pa_mainloop_iterate(ps->loop, 0, NULL);
    return NULL;
}

static void _corkComplete(pa_stream *s, int success, void *userdata)
{
    (void) s;
    (void) userdata;

    // NOTE: success is zero when un-corking a pause of more than 30 sec.
#ifdef DEBUG
    if (! success)
        fprintf(stderr /*_errStream*/, "pa_stream_cork failed\n");
#else
    (void) success;
#endif
}

static int sysaudio_startVoice(FaunVoice *voice)
{
    pa_stream_cork(PS->stream, 0, _corkComplete, NULL);
    return 1;
}

static int sysaudio_stopVoice(FaunVoice *voice)
{
    PulseSession* ps = PS;
    pa_stream_cork(ps->stream, 1, _corkComplete, NULL);

    // Must run loop for cork to take effect.
    pa_mainloop_iterate(ps->loop, 0, NULL);
    return 1;
}
