/*
  Faun WASAPI backend
*/

#define COBJMACROS
#include <mmdeviceapi.h>
#include <audioclient.h>

const CLSID CLSID_MMDeviceEnumerator = {
    0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}
};
const IID   IID_IMMDeviceEnumerator = {
    0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}
};
const IID   IID_IAudioClient = {
    0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}
};
const IID   IID_IAudioRenderClient = {
    0xf294acfc, 0x3146, 0x4483, {0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2}
};


struct {
    IMMDevice* mmdev;
    IAudioClient* client;
    IAudioRenderClient* render;
    UINT32 rbufSize;            // Frame count.
    WAVEFORMATEX format;
    char errorMsg[80];
}
waSession;


static const char* waError(const char* msg, HRESULT hr)
{
    sprintf(waSession.errorMsg, "%s (0x%08lx)", msg, hr);
    return waSession.errorMsg;
}

static void sysaudio_close();

static const char* sysaudio_open(const char* appName)
{
    IMMDeviceEnumerator* denum;
    void* ptr;
    //WAVEFORMATEX* closest = NULL;
    WAVEFORMATEX* fmt;
    HRESULT hr;
    (void) appName;


    waSession.mmdev  = NULL;
    waSession.client = NULL;
    waSession.render = NULL;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr))
        return "COM initialize failed";

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL,
                          CLSCTX_INPROC_SERVER, &IID_IMMDeviceEnumerator, &ptr);
    if (FAILED(hr))
        return "Create MMDeviceEnumerator failed";

    denum = ptr;
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(denum, eRender,
                                                eMultimedia, &waSession.mmdev);
    IMMDeviceEnumerator_Release(denum);
    if (FAILED(hr))
        return "GetDefaultAudioEndpoint failed";

    hr = IMMDevice_Activate(waSession.mmdev, &IID_IAudioClient,
                            CLSCTX_INPROC_SERVER, NULL, &ptr);
    if(FAILED(hr)) {
        IMMDevice_Release(waSession.mmdev);
        waSession.mmdev = NULL;
        return "IMMDevice_Activate failed";
    }

    waSession.client = ptr;
    //get_device_name_and_guid(waSession.mmdev, &deviceName, NULL);

    fmt = &waSession.format;
    fmt->wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
    fmt->nChannels       = 2;
    fmt->nSamplesPerSec  = 44100;
    fmt->nAvgBytesPerSec = 2 * 44100 * sizeof(float);
    fmt->wBitsPerSample  = 32;
    fmt->nBlockAlign     = (fmt->nChannels * fmt->wBitsPerSample) / 8;
    fmt->cbSize          = 0;

#if 0
    hr = IAudioClient_IsFormatSupported(waSession.client,
                                        AUDCLNT_SHAREMODE_SHARED,
                                        fmt, &closest);
    if (hr != S_OK) {
        CoTaskMemFree(closest);
        sysaudio_close();
        return "Audio device does not support float sample format";
    }
#endif

    return NULL;
}

static void sysaudio_close()
{
    if (waSession.client) {
        IAudioClient_Release(waSession.client);
        waSession.client = NULL;
    }

    if (waSession.mmdev) {
        IMMDevice_Release(waSession.mmdev);
        waSession.mmdev = NULL;
    }

    CoUninitialize();
}

static const char* sysaudio_allocVoice(FaunVoice* voice, int updateHz,
                                       const char* appName)
{
    REFERENCE_TIME bufTime = 50 * 10000;    // 50 ms of latency
    //REFERENCE_TIME minPeriod;
    BYTE* rbuf;
    void* ptr;
    UINT32 frameCount;
    HRESULT hr;
    (void) voice;
    (void) updateHz;
    (void) appName;

    hr = IAudioClient_Initialize(waSession.client, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_NOPERSIST |
                                 AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                 AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                 bufTime, 0, &waSession.format, NULL);
    if (FAILED(hr))
        return waError("AudioClient Initialize failed", hr);

#if 0
    hr = IAudioClient_GetDevicePeriod(waSession.client, &minPeriod, NULL);
    if (FAILED(hr))
        return waError("GetDevicePeriod failed", hr);
    printf("KR minPeriod: %I64d\n", minPeriod);
#endif

    hr = IAudioClient_GetBufferSize(waSession.client, &frameCount);
    if (FAILED(hr))
        return waError("GetBufferSize failed", hr);
    waSession.rbufSize = frameCount;

    hr = IAudioClient_GetService(waSession.client, &IID_IAudioRenderClient,
                                 &ptr);
    if (FAILED(hr))
        return waError("Get AudioRenderClient failed", hr);
    waSession.render = ptr;

    // Fill buffer with silence.
    hr = IAudioRenderClient_GetBuffer(waSession.render, frameCount, &rbuf);
    if (FAILED(hr))
        return waError("GetBuffer failed", hr);
    IAudioRenderClient_ReleaseBuffer(waSession.render, frameCount,
                                     AUDCLNT_BUFFERFLAGS_SILENT);

    hr = IAudioClient_Start(waSession.client);
    if (FAILED(hr))
        return waError("AudioClient start failed", hr);

    return NULL;
}

static void sysaudio_freeVoice(FaunVoice *voice)
{
    (void) voice;

    if (waSession.render) {
        IAudioRenderClient_Release(waSession.render);
        waSession.render = NULL;
    }
    //voice->backend = NULL;
}

static const char* sysaudio_write(FaunVoice* voice, const void* data,
                                  uint32_t len)
{
    BYTE* rbuf;
    HRESULT hr;
    UINT32 padding;
    UINT32 framesAvail;
    UINT32 flen = len / waSession.format.nBlockAlign;
    DWORD ms;
    (void) voice;


    while (1) {
        hr = IAudioClient_GetCurrentPadding(waSession.client, &padding);
        if (FAILED(hr))
            return waError("GetCurrentPadding failed", hr);

        framesAvail = waSession.rbufSize - padding;
        if (framesAvail >= flen)
            break;

        ms = (flen - framesAvail) * 1000 / voice->mix.rate;
        //printf("  Sleep(%ld)\n", ms);
        Sleep(ms);
    }

    hr = IAudioRenderClient_GetBuffer(waSession.render, flen, &rbuf);
    if (FAILED(hr))
        return waError("GetBuffer failed", hr);
    memcpy(rbuf, data, len);
    IAudioRenderClient_ReleaseBuffer(waSession.render, flen, 0);
    return NULL;
}

static int sysaudio_startVoice(FaunVoice *voice)
{
    (void) voice;
    IAudioClient_Start(waSession.client);
    return 1;
}

static int sysaudio_stopVoice(FaunVoice *voice)
{
    (void) voice;
    IAudioClient_Stop(waSession.client);
    return 0;
}
