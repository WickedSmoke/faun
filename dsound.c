/*
  Faun DirectSound backend
*/

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#endif

#ifndef WINVER
#define WINVER 0x0501
#endif

#include <stdio.h>
//#include <string.h>
#include <windows.h>
#define DIRECTSOUND_VERSION 0x0800
#include <dsound.h>


// Divide the ds_buffer into three sections.
#define SEC_COUNT   3

static LPDIRECTSOUND8 dsDevice;
static char dsErrorMsg[80];

/* Voice information for DirectSound */
typedef struct {
    DSBUFFERDESC desc;
    WAVEFORMATEX waveFmt;
    LPDIRECTSOUNDBUFFER ds_buffer;
    LPDIRECTSOUNDBUFFER8 ds8_buffer;
    int section;
} DSVoice;


static const char* dsoundError(const char* msg, HRESULT hr)
{
    const char* estr = NULL;
    switch (hr) {
        case DSERR_ALLOCATED:       estr = "DSERR_ALLOCATED";       break;
        case DSERR_BADFORMAT:       estr = "DSERR_BADFORMAT";       break;
        case DSERR_BUFFERTOOSMALL:  estr = "DSERR_BUFFERTOOSMALL";  break;
        case DSERR_CONTROLUNAVAIL:  estr = "DSERR_CONTROLUNAVAIL";  break;
        case DSERR_DS8_REQUIRED:    estr = "DSERR_DS8_REQUIRED";    break;
        case DSERR_INVALIDCALL:     estr = "DSERR_INVALIDCALL";     break;
        case DSERR_INVALIDPARAM:    estr = "DSERR_INVALIDPARAM";    break;
        case DSERR_NOAGGREGATION:   estr = "DSERR_NOAGGREGATION";   break;
        case DSERR_OUTOFMEMORY:     estr = "DSERR_OUTOFMEMORY";     break;
        case DSERR_UNINITIALIZED:   estr = "DSERR_UNINITIALIZED";   break;
        case DSERR_UNSUPPORTED:     estr = "DSERR_UNSUPPORTED";     break;
    }
    if (estr)
        sprintf(dsErrorMsg, "%s (%s)", msg, estr);
    else
        sprintf(dsErrorMsg, "%s (%ld)", msg, hr);
    return dsErrorMsg;
}

/* The open method starts up the driver and should lock the device, using the
   previously set parameters, or defaults. It shouldn't need to start sending
   audio data to the device yet, however. */
static const char* sysaudio_open(const char* appName)
{
    HRESULT hr;
    HWND win;
    (void) appName;

    // Use default device.
    hr = DirectSoundCreate8(NULL, &dsDevice, NULL);
    if (FAILED(hr))
        return dsoundError("DirectSoundCreate8 failed", hr);

    win = GetForegroundWindow();
    if (win == NULL)
        win = GetDesktopWindow();

    // DSSCL_PRIORITY
    hr = IDirectSound8_SetCooperativeLevel(dsDevice, win, DSSCL_NORMAL);
    if (FAILED(hr))
        return dsoundError("SetCooperativeLevel failed", hr);

    return NULL;
}


/* The close method should close the device, freeing any resources, and allow
   other processes to use the device */
static void sysaudio_close()
{
    IDirectSound8_Release(dsDevice);
}


/* The allocate_voice method should grab a voice from the system, and allocate
   any data common to streaming and non-streaming sources. */
static const char* sysaudio_allocVoice(FaunVoice* voice, int updateHz,
                                       const char* appName)
{
    DSVoice* ds;
    HRESULT hr;
    int bitsPerSample = 16;
    int channels;
    int bufferSize;
    (void) appName;

    ds = (DSVoice*) calloc(1, sizeof(DSVoice));
    if (! ds)
        return "DSVoice allocate failed";

    voice->backend = ds;

    channels = faun_channelCount(voice->mix.chanLayout);
    bufferSize = (SEC_COUNT*44100/updateHz) * (bitsPerSample/8) * channels;

    ds->waveFmt.wFormatTag      = WAVE_FORMAT_PCM;
    ds->waveFmt.nChannels       = channels;
    ds->waveFmt.nSamplesPerSec  = voice->mix.rate;
    ds->waveFmt.nBlockAlign     = channels * (bitsPerSample/8);
    ds->waveFmt.nAvgBytesPerSec = ds->waveFmt.nBlockAlign *
                                  voice->mix.rate;
    ds->waveFmt.wBitsPerSample  = bitsPerSample;
    ds->waveFmt.cbSize = 0;

    ds->desc.dwSize         = sizeof(DSBUFFERDESC);
    ds->desc.dwFlags        = /*DSBCAPS_LOCSOFTWARE |*/
                              DSBCAPS_GETCURRENTPOSITION2 |
                              DSBCAPS_GLOBALFOCUS;
    ds->desc.dwBufferBytes  = bufferSize;
    ds->desc.dwReserved     = 0;
    ds->desc.lpwfxFormat    = &ds->waveFmt;
    ds->desc.guid3DAlgorithm = DS3DALG_DEFAULT;

    hr = IDirectSound8_CreateSoundBuffer(dsDevice, &ds->desc, &ds->ds_buffer,
                                         NULL);
    if (FAILED(hr))
        return dsoundError("CreateSoundBuffer failed", hr);

    // NOTE: When compiled as C (rather than C++) IID_ arguments require '&'.
    hr = IDirectSoundBuffer_QueryInterface(ds->ds_buffer,
                                           &IID_IDirectSoundBuffer8,
                                           (LPVOID*) &ds->ds8_buffer);
    if (FAILED(hr))
        return dsoundError("QueryInterface failed", hr);

    // Fill buffer with silence.
    {
    LPVOID part1, part2;
    DWORD len1, len2;

    hr = IDirectSoundBuffer8_Lock(ds->ds8_buffer, 0, bufferSize,
                                  &part1, &len1, &part2, &len2,
                                  DSBLOCK_ENTIREBUFFER);
    if (FAILED(hr))
        return dsoundError("Buffer Lock failed", hr);

    memset(part1, 0, len1);
    if (len2)
        memset(part2, 0, len2);
    IDirectSoundBuffer8_Unlock(ds->ds8_buffer, part1, len1, part2, len2);
    }

    IDirectSoundBuffer8_SetVolume(ds->ds8_buffer, DSBVOLUME_MAX);
    IDirectSoundBuffer8_Play(ds->ds8_buffer, 0, 0, DSBPLAY_LOOPING);
    ds->section = 0;

    return NULL;
}

/* The deallocate_voice method should free the resources for the given voice,
   but still retain a hold on the device. The voice should be stopped and
   unloaded by the time this is called */
static void sysaudio_freeVoice(FaunVoice *voice)
{
    DSVoice* ds = (DSVoice*) voice->backend;
    if (ds->ds8_buffer) {
        IDirectSoundBuffer8_Release(ds->ds8_buffer);
        ds->ds8_buffer = NULL;
    }
    free(ds);
    voice->backend = NULL;
}


static inline void convF32_S16(int16_t* dst, int16_t* end, const float* src)
{
    while (dst != end)
        *dst++ = (int16_t) (*src++ * 32767.0f);
}


static const char* sysaudio_write(FaunVoice* voice, const void* data,
                                  uint32_t len)
{
    DSVoice* ds = (DSVoice*) voice->backend;
    DWORD playPos = 0;
    DWORD writePos;
    HRESULT hr;
    LPVOID part1, part2;
    DWORD len1, len2;
    uint32_t secBytes = ds->desc.dwBufferBytes / SEC_COUNT;
    int playSection;
    int wait = 0;

    if (len != secBytes*2)
        return "Unexpected sysaudio_write len";

    do
    {
        if (wait)
            Sleep(2);
        else
            ++wait;
        IDirectSoundBuffer8_GetCurrentPosition(ds->ds8_buffer,
                                               &playPos, &writePos);
        playSection = playPos * SEC_COUNT / ds->desc.dwBufferBytes;
    }
    while (playSection == ds->section);


    hr = IDirectSoundBuffer8_Lock(ds->ds8_buffer, ds->section * secBytes,
                                  secBytes, &part1, &len1, &part2, &len2, 0);
    if (FAILED(hr))
        return dsoundError("Buffer Lock failed", hr);
    {
    const float* fdat = (const float*) data;
    int16_t* sp = (int16_t*) part1;
    int samples1 = len1 / sizeof(int16_t);
    convF32_S16(sp, sp + samples1, fdat);
    if (len2) {
        sp = (int16_t*) part2;
        convF32_S16(sp, sp + len2 / sizeof(int16_t), fdat + samples1);
    }
    }
    IDirectSoundBuffer8_Unlock(ds->ds8_buffer, part1, len1, part2, len2);

    if (ds->section == SEC_COUNT-1)
        ds->section = 0;
    else
        ++ds->section;
    return NULL;
}

static int sysaudio_startVoice(FaunVoice *voice)
{
    DSVoice *ds = (DSVoice*) voice->backend;
    if (ds->ds8_buffer) {
        IDirectSoundBuffer8_Play(ds->ds8_buffer, 0, 0, DSBPLAY_LOOPING);
        return 1;
    }
    return 0;
}

static int sysaudio_stopVoice(FaunVoice* voice)
{
    DSVoice *ds = (DSVoice*) voice->backend;
    if (ds->ds8_buffer) {
        IDirectSoundBuffer8_Stop(ds->ds8_buffer);
        return 1;
    }
    return 0;
}

/*
static int sysaudio_isPlaying(const FaunVoice *voice)
{
    DSVoice *ds = (DSVoice*) voice->backend;
    DWORD status;

    if (! ds)
        return 0;

    IDirectSoundBuffer8_GetStatus(ds->ds8_buffer, &status);
    return (status & DSBSTATUS_PLAYING);
}
*/
