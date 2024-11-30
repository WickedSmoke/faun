/*  ___________
    \_   _____/____   __ __  ____
     |    __) \__  \ |  |  \/    \
     |    \    / __ \|  |  /   |  \
     \__  /   (____  /____/|___|  /
        \/         \/           \/

  Faun - A high-level C audio library

  Copyright (c) 2022-2024 Karl Robillard

  Permission is hereby granted, free of charge, to any person obtaining a
  copy of this software and associated documentation files (the "Software"),
  to deal in the Software without restriction, including without limitation
  the rights to use, copy, modify, merge, publish, distribute, sublicense,
  and/or sell copies of the Software, and to permit persons to whom the
  Software is furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
  THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
  DEALINGS IN THE SOFTWARE.
*/

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "internal.h"
#include "faun.h"

#if __STDC_VERSION__ < 201112L || defined(__STDC_NO_ATOMICS__)
#error "Atomic support is required for playback ids!"
#else
#include <stdatomic.h>
#endif

#ifdef ANDROID
#include "sys_aaudio.c"
#elif defined(__linux__)
#include "sys_pulseaudio.c"
#elif defined(_WIN32)
#include "sys_wasapi.c"
#else
#error "Unsupported system"
#endif

#include <vorbis/codec.h>
#include <vorbis/vorbisfile.h>

#ifdef CAPTURE
#include "wav_write.c"
FILE* wfp = NULL;
int endOnSignal = 0;
int endCapture = 0;
#endif

#if 0
#define REPORT_STREAM(st,msg) \
    printf("FAUN strm %x: %s\n",_asource[st->sindex].serialNo,msg)
#define REPORT_BUF(fmt, ...) printf(fmt, ##__VA_ARGS__)
#define REPORT_MIX(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define REPORT_STREAM(st,msg)
#define REPORT_BUF(msg, ...)
#define REPORT_MIX(msg, ...)
#endif

typedef struct {
#ifdef GLV_ASSET_H
    struct AssetFile asset;
#define cfile   asset.fp
#else
    FILE* cfile;
#endif
    uint32_t offset;
    uint32_t size;
}
FileChunk;

typedef struct {
    uint8_t  op;
    uint8_t  select;
    uint16_t ext;
    union {
        uint16_t u16[8];
        uint32_t u32[4];
        float f[4];
    } arg;
}
CommandA;

typedef struct {
    uint8_t  op;
    uint8_t  select;
    uint16_t ext;
    FileChunk chunk;
}
CommandF;

enum FaunCmd {
    CMD_QUIT,
    CMD_SUSPEND,
    CMD_RESUME,
    CMD_PROGRAM,
    CMD_PROGRAM_END,
    CMD_PROGRAM_MID,
    CMD_PROGRAM_BEG,
    CMD_SET_BUFFER,
    CMD_BUFFERS_FREE,
    CMD_PLAY_SOURCE,
    CMD_PLAY_SOURCE_VOL,
    CMD_OPEN_STREAM_SIZE,
    CMD_OPEN_STREAM,
    CMD_PLAY_STREAM_PART,
    CMD_VOLUME_VARY,

    CMD_CON_START,
    CMD_CON_STOP,
    CMD_CON_RESUME,
    CMD_CON_FADE_OUT,

    CMD_PARAM_VOLUME,
    CMD_PARAM_FADE_PERIOD,
    CMD_PARAM_END_TIME,

    CMD_COUNT
};

#define MSG_SIZE    20
#define PROG_CHEAD  3

typedef struct {
    uint8_t code[FAUN_PROGRAM_MAX];
    int pc;
    int used;
    uint16_t running;
    uint16_t si;
    uint32_t waitPos;
}
FaunProgram;

enum SourceState {
    SS_UNUSED,
    SS_PLAYING,
    SS_STOPPED
};

#define NUL_PLAY_ID     0
#define QACTIVE_NONE    0xffff
#define END_POS_NONE    0x7fffffff
#define SOURCE_QUEUE_SIZE   4
#define BID_PACKED      0x3ff
#define SOURCE_ID(src)  (src->serialNo & 0xff)

// Internal FaunPlayMode flags
#define PLAY_TARGET_VOL 0x4000
#define END_AFTER_FADE  0x8000

typedef struct {
    uint16_t state;         // SourceState
    uint16_t bufUsed;       // Number of buffers in queue.
    uint16_t qtail;         // Queue index of append position.
    uint16_t qhead;
    uint16_t qactive;       // Queue index of currently playing buffer.
    uint16_t mode;

    // These are ordered to match fadeTriplet.
    float    gainL;         // Current volume.
    float    gainR;
    float    fadeL;         // Gain delta per frame.
    float    fadeR;
    float    targetL;       // Target volume.
    float    targetR;

    float    playVolume;    // FAUN_VOLUME, used when play begins.
    float    fadePeriod;    // FAUN_FADE_PERIOD
    uint32_t serialNo;
    uint32_t playPos;       // Frame position in current buffer.
    uint32_t framesOut;     // Total frames played.
    uint32_t endPos;        // User specified end frame. FAUN_END_TIME
    uint32_t fadePos;       // Frame to begin fade out.
    FaunBuffer* bufferQueue[SOURCE_QUEUE_SIZE];
}
FaunSource;


static const uint8_t faun_formatSize[FAUN_FORMAT_COUNT] = { 1, 2, 3, 4 };
static FILE* _errStream;


static void faun_sourceInit(FaunSource* src, int si)
{
    memset(src, 0, sizeof(FaunSource));
    src->qactive = QACTIVE_NONE;
    src->gainL = src->gainR = 1.0f;
    //src->fadeL = src->fadeR = 0.0f;
    src->targetL = src->targetR =
    src->playVolume = 1.0f;
    src->fadePeriod = 1.5f;
    src->serialNo = si;
    src->endPos =
    src->fadePos = END_POS_NONE;
}


static void faun_setBuffer(FaunSource* src, FaunBuffer* buf)
{
    src->bufUsed = src->qtail = 1;
    src->qhead = src->qactive = 0;
    src->bufferQueue[0] = buf;
}


static void faun_sourceResetQueue(FaunSource* src)
{
    src->bufUsed = src->qtail = src->qhead = 0;
    src->qactive = QACTIVE_NONE;
}


static void faun_queueBuffer(FaunSource* src, FaunBuffer* buf)
{
    int i;
    if (src->bufUsed < SOURCE_QUEUE_SIZE) {
        src->bufUsed++;
        i = src->qtail;
        src->bufferQueue[i] = buf;
        if (src->qactive == QACTIVE_NONE)
            src->qactive = i;
        if (++i == SOURCE_QUEUE_SIZE)
            i = 0;
        src->qtail = i;
    } else
        fprintf(_errStream, "Faun source queue full (%x)\n", src->serialNo);
}


/*
 * Dequeue the next played buffer.
 * Return buffer pointer or NULL if there are none finished playing in the
 * queue.
 */
static FaunBuffer* faun_processedBuffer(FaunSource* src)
{
    FaunBuffer* ptr;
    int i;
    if (src->bufUsed && src->qactive != src->qhead) {
        i = src->qhead;
        ptr = src->bufferQueue[i];
        if (++i == SOURCE_QUEUE_SIZE)
            i = 0;
        src->qhead = i;
        src->bufUsed--;
        return ptr;
    }
    return NULL;
}


void faun_reserve(FaunBuffer* buf, int frames)
{
    if (buf->avail < (uint32_t) frames) {
        buf->sample.ptr = realloc(buf->sample.ptr,
                                  frames * faun_formatSize[buf->format] *
                                           faun_channelCount(buf->chanLayout));
        buf->avail = frames;
    }
}


static void faun_allocBufferSamples(FaunBuffer* buf, int fmt, int chan,
                                    int rate, int frames)
{
    free(buf->sample.ptr);
    buf->sample.ptr = malloc(frames * faun_formatSize[fmt] *
                                      faun_channelCount(chan));
    buf->avail = frames;
    buf->used  = 0;

    buf->rate = rate;
    buf->format = fmt;
    buf->chanLayout = chan;
}


static void faun_freeBufferSamples(int n, FaunBuffer* buf)
{
    int i;
    for (i = 0; i < n; ++i) {
        free(buf->sample.ptr);
        buf->sample.ptr = NULL;
        ++buf;
    }
}


#define STREAM_BUFFERS  4
#define SEGMENT_SET(st) st->sampleLimit

typedef struct {
    FaunBuffer  buffers[STREAM_BUFFERS];
    int         bufAvail;
    int16_t     feed;
    int16_t     sindex;
    double      start;
    uint32_t    sampleCount;    // Number of samples read
    uint32_t    sampleLimit;    // Number of samples to buffer before ending
    FileChunk   chunk;
    OggVorbis_File vf;
    vorbis_info* vinfo;
}
StreamOV;


enum AudioState
{
    AUDIO_DOWN,
    AUDIO_UP,
    AUDIO_THREAD_UP
};

enum ReadOggStatus {
    RSTAT_ERROR = 1,
    RSTAT_EOF   = 2,
    RSTAT_DATA  = 4
};

#define BUFFER_MAX  256
#define SOURCE_MAX  32
#define STREAM_MAX  6
#define PEXEC_MAX   16

static int _audioUp = AUDIO_DOWN;
static int _bufferLimit;
static int _sourceLimit;
static int _streamLimit;
static int _pexecLimit;
static uint32_t _playSerialNo;
static FaunVoice   _voice;
static FaunBuffer* _abuffer = NULL;
static FaunSource* _asource = NULL;
static StreamOV*  _stream = NULL;
static FaunProgram* _pexec = NULL;
static _Atomic uint32_t* _playbackId = NULL;
static atomic_flag _pidLock;

//----------------------------------------------------------------------------

#include "wav_read.c"

static void _allocBufferVoice(FaunBuffer*, int);

#ifdef USE_FLAC
#include "FlacReader.c"
#endif

#ifdef USE_SFX_GEN
#define CONFIG_SFX_NO_FILEIO
#define CONFIG_SFX_NO_GENERATORS
#define SINGLE_FORMAT   3
#include "sfx_gen.c"

#define well512_init    faun_randomSeed
#define well512_genU32  faun_random
#include "well512.c"

Well512 _rng;

int sfx_random(int range) {
    return faun_random(&_rng) % range;
}

static void convertMono(float*, float*, float**);

static void faun_generateSfx(FaunBuffer* buf, const SfxParams* sp)
{
    SfxSynth* synth;
    float* dst;
    float* src;
    uint32_t frames;

    synth = sfx_allocSynth(SFX_F32, 44100, 6);
    faun_randomSeed(&_rng, sp->randSeed);
    frames = sfx_generateWave(synth, sp);

    _allocBufferVoice(buf, frames);
    dst = buf->sample.f32;
    src = synth->samples.f;
    convertMono(dst, dst + frames*2, &src);
    buf->used = frames;

    free(synth);
}
#endif

#define ID_FLAC MAKE_ID('f','L','a','C')
#define ID_OGGS MAKE_ID('O','g','g','S')
#define ID_RFX_ MAKE_ID('r','F','X',' ')

//----------------------------------------------------------------------------

static size_t chunk_fread(void* buf, size_t size, size_t nmemb, void* fh)
{
    //printf("OV fread %ld %ld\n", size, nmemb);
    return fread(buf, size, nmemb, ((FileChunk*) fh)->cfile);
}

static int chunk_fseek(void* fh, ogg_int64_t offset, int whence)
{
    FileChunk* fc = (FileChunk*) fh;
    //printf("OV seek %ld %d\n", offset, whence);
    if (whence == SEEK_SET)
        offset += fc->offset;
    else if (whence == SEEK_END && fc->size) {
        whence = SEEK_SET;
        offset += fc->offset + fc->size;
    }
    return fseek(fc->cfile, offset, whence);
}

static int chunk_fclose(void* fh)
{
    FileChunk* fc = (FileChunk*) fh;
    int ok = fclose(fc->cfile);
    fc->cfile = NULL;
    return ok;
}

static long chunk_ftell(void* fh)
{
    FileChunk* fc = (FileChunk*) fh;
    long pos = ftell(fc->cfile);
    long start = (long) fc->offset;
    if (pos < start)
        return -1;
    //printf("OV ftell %ld %ld\n", pos, pos-start);
    return pos - start;
}

static ov_callbacks chunkMethods = {
    chunk_fread, chunk_fseek, chunk_fclose, chunk_ftell
};

static ov_callbacks chunkMethodsNoClose = {
    chunk_fread, chunk_fseek, NULL, chunk_ftell
};

static int _readOgg(StreamOV* st, FaunBuffer* buffer);

//----------------------------------------------------------------------------

// Allocate a buffer with attributes that match the voice mixing buffer.
static void _allocBufferVoice(FaunBuffer* buf, int frames)
{
    faun_allocBufferSamples(buf, FAUN_F32, FAUN_CHAN_2, _voice.mix.rate,
                            frames);
}

static void convS16_F32(float* dst, const int16_t* src, int frames, int rate,
                        int channels)
{
    const int16_t* end = src + frames * channels;
    float ls, rs;

    if (channels == 1) {
        if (rate == 22050) {
            for (; src != end; ++src) {
                ls = *src / 32767.0f;
                *dst++ = ls;
                *dst++ = ls;
                *dst++ = ls;
                *dst++ = ls;
            }
        } else {
            for (; src != end; ++src) {
                ls = *src / 32767.0f;
                *dst++ = ls;
                *dst++ = ls;
            }
        }
    } else if (channels >= 2) {
        if (rate == 22050) {
            for (; src != end; src += channels) {
                ls = src[0] / 32767.0f;
                rs = src[1] / 32767.0f;
                *dst++ = ls;
                *dst++ = rs;
                *dst++ = ls;
                *dst++ = rs;
            }
        } else {
            for (; src != end; src += channels) {
                *dst++ = src[0] / 32767.0f;
                *dst++ = src[1] / 32767.0f;
            }
        }
    }
}

#ifdef USE_LOAD_MEM
static void convF32_F32(float* dst, const float* src, int frames, int rate,
                        int channels)
{
    const float* end = src + frames * channels;
    float ls, rs;

    if (channels == 1) {
        if (rate == 22050) {
            for (; src != end; ++src) {
                ls = *src;
                *dst++ = ls;
                *dst++ = ls;
                *dst++ = ls;
                *dst++ = ls;
            }
        } else {
            for (; src != end; ++src) {
                ls = *src;
                *dst++ = ls;
                *dst++ = ls;
            }
        }
    } else if (channels >= 2) {
        if (rate == 22050) {
            for (; src != end; src += channels) {
                ls = src[0];
                rs = src[1];
                *dst++ = ls;
                *dst++ = rs;
                *dst++ = ls;
                *dst++ = rs;
            }
        } else {
            for (; src != end; src += channels) {
                *dst++ = src[0];
                *dst++ = src[1];
            }
        }
    }
}
#endif

/*
  Read buffer sample data from a file.

  The existing buf->sample data is freed first, so the sample.ptr must
  be valid or NULL.

  Return error message or NULL if successful.
*/
static const char* faun_readBuffer(FaunBuffer* buf, FILE* fp,
                                   uint32_t offset, uint32_t size)
{
    WavHeader wh;
    const char* error = NULL;
    uint32_t frames = 0;
    const int wavReadLen = 20;  // wav_readHeader() reads 20 bytes.
    int err;

    if (offset)
        fseek(fp, offset, SEEK_SET);

    err = wav_readHeader(fp, &wh);
    if (err == 0) {
        void* readBuf;
        size_t n;
        uint32_t wavFrames;

        //wav_dumpHeader(stdout, &wh, NULL, "  ");

        if (wh.sampleRate != 44100 && wh.sampleRate != 22050)
            return "WAVE sample rate is unsupported";
#ifdef USE_LOAD_MEM
        if (wh.format == WAV_IEEE_FLOAT) {
            if (wh.bitsPerSample != 32)
                return "WAVE float bits per sample is not 32";
        } else
#endif
        if (wh.bitsPerSample != 16)
            return "WAVE bits per sample is not 16";

        frames = wavFrames = wav_sampleCount(&wh);
        if (wh.sampleRate == 22050)
            frames *= 2;
        _allocBufferVoice(buf, frames);

        readBuf = malloc(wh.dataSize);
        n = fread(readBuf, 1, wh.dataSize, fp);
        if (n != wh.dataSize) {
            error = "WAVE fread failed";
        } else {
            buf->used = frames;
#ifdef USE_LOAD_MEM
            if (wh.format == WAV_IEEE_FLOAT)
                convF32_F32(buf->sample.f32, (float*) readBuf, wavFrames,
                            wh.sampleRate, wh.channels);
            else
#endif
                convS16_F32(buf->sample.f32, (int16_t*) readBuf, wavFrames,
                            wh.sampleRate, wh.channels);
        }
        free(readBuf);
    }
    else if (err == WAV_ERROR_ID)
    {
      if (wh.idRIFF == ID_OGGS)
      {
        StreamOV os;
        int status;

        // Minimal version of stream_init() to use _readOgg().
        os.sampleCount = 0;

        os.chunk.cfile  = fp;
        os.chunk.offset = offset;
        os.chunk.size   = size;

        if (ov_open_callbacks(&os.chunk, &os.vf, (char*) &wh, wavReadLen,
                              chunkMethodsNoClose) < 0)
        {
            error = "Ogg open failed";
        }
        else
        {
            os.vinfo = ov_info(&os.vf, -1);
            frames = ov_pcm_total(&os.vf, -1);
            if (os.vinfo->rate == 22050)
                frames *= 2;
            //printf("FAUN ogg frame:%d chan:%d rate:%ld\n",
            //       frames, os.vinfo->channels, os.vinfo->rate);
            _allocBufferVoice(buf, frames);
            status = _readOgg(&os, buf);
            if (status != RSTAT_DATA)
                error = "Ogg read failed";

            ov_clear(&os.vf);
        }
      }
      else if (wh.idRIFF == ID_FLAC)
      {
#ifndef USE_FLAC
        error = "Faun built without FLAC support";
#elif USE_FLAC == 2
        error = foxenFlacDecode(fp, size, buf, &wh, wavReadLen);
#else
        fseek(fp, -wavReadLen, SEEK_CUR);
        error = libFlacDecode(fp, size, buf);
#endif
      }
#ifdef USE_SFX_GEN
      else if (wh.idRIFF == ID_RFX_)
      {
        SfxParams sfx;
        int version = ((uint16_t*) &wh)[2];
        if (version != 200)
            error = "rFX file version not supported";
        else {
            fseek(fp, offset + 8, SEEK_SET);
            if (fread(&sfx, 1, sizeof(SfxParams), fp) == sizeof(SfxParams))
                faun_generateSfx(buf, &sfx);
            else
                error = "rFX fread failed";
        }
      }
#endif
    }

#if 0
    fp = wav_open("/tmp/out.wav", 44100, 16, 2);
    wav_write(fp, buf->sample.f32, buf->used*2);
    wav_close(fp);
#endif

    return error;
}

static void faun_deactivate(FaunSource* src, int si)
{
    src->qactive = QACTIVE_NONE;
    src->state = SS_UNUSED;

    // Clear playback id (unless an incoming command has already changed it).
    while (atomic_flag_test_and_set(&_pidLock)) {}
    if (_playbackId[si] == src->serialNo)
        _playbackId[si] = NUL_PLAY_ID;
    atomic_flag_clear(&_pidLock);
}

// Abort all sources playing a freed buffer.
static void faun_detachBuffers()
{
    FaunSource* src;
    int i;
    for (i = 0; i < _sourceLimit; ++i) {
        src = _asource + i;
        if (src->qactive != QACTIVE_NONE) {
            // Only the current buffer is checked; any queued buffers
            // will be skipped during the "Advance play positions" phase.

            if (! src->bufferQueue[src->qactive]->sample.ptr) {
                faun_deactivate(src, i);
            }
        }
    }
}


//----------------------------------------------------------------------------

static void stream_init(StreamOV* st, int id)
{
    memset(&st->buffers, 0, sizeof(FaunBuffer) * STREAM_BUFFERS);
    st->feed = 0;
    st->sindex = id;

#ifdef GLV_ASSET_H
    memset(&st->asset, 0, sizeof(struct AssetFile));
#else
    st->chunk.cfile = NULL;
#endif
}

static void stream_free(StreamOV* st)
{
    faun_freeBufferSamples(STREAM_BUFFERS, st->buffers);
}

static void stream_closeFile(StreamOV* st)
{
    ov_clear(&st->vf);      // Closes st->chunk.cfile for us.
    st->chunk.cfile = NULL;
#ifdef _ANDROID_
    glv_assetClose(&st->asset);
#endif
}


static void convertStereoHR(float* dst, float* end, float** src)
{
    const float* ls = src[0];
    const float* rs = src[1];
    float L, R;
    while (dst != end) {
        L = *ls++;
        R = *rs++;
        *dst++ = L;
        *dst++ = R;
        *dst++ = L;
        *dst++ = R;
    }
}

// Interleave the separate channel data.
static void convertStereo(float* dst, float* end, float** src)
{
    const float* ls = src[0];
    const float* rs = src[1];
    while (dst != end) {
        *dst++ = *ls++;
        *dst++ = *rs++;
    }
}

static void convertMonoHR(float* dst, float* end, float** src)
{
    const float* c0 = src[0];
    float L, R;
    while (dst != end) {
        L = *c0;
        R = *c0++;
        *dst++ = L;
        *dst++ = R;
        *dst++ = L;
        *dst++ = R;
    }
}

static void convertMono(float* dst, float* end, float** src)
{
    const float* c0 = src[0];
    float C;
    while (dst != end) {
        C = *c0++;
        *dst++ = C;
        *dst++ = C;
    }
}

typedef void (*StreamConvertFunc)(float*, float*, float**);


/*
  Decode some audio, copy it into a buffer, and update sampleCount.
  Returns a mask of ReadOggStatus bits.
*/
static int _readOgg(StreamOV* st, FaunBuffer* buffer)
{
    float** oggPcm;
    float* dst;
    StreamConvertFunc convert;
    int status;
    int bitstream;
    int count;
    int readFrames = buffer->avail;
    int readSamples;
    long amt = 0;
    int halfRate = (st->vinfo->rate == (int) buffer->rate/2);

    if (st->vinfo->channels > 1)
        convert = halfRate ? convertStereoHR : convertStereo;
    else
        convert = halfRate ? convertMonoHR : convertMono;

    for (count = 0; count < readFrames; count += amt)
    {
        // Decode one vorbis packet.  Samples are decoded internally to float
        // so ov_read_float is faster than ov_read.
        readSamples = readFrames - count;
        if (halfRate)
            readSamples /= 2;
        amt = ov_read_float(&st->vf, &oggPcm, readSamples, &bitstream);
        if (amt < 1)
            break;
        if (halfRate)
            amt *= 2;

        dst = buffer->sample.f32 + count*2;
        convert(dst, dst + amt*2, oggPcm);
    }

    if( amt < 0 )
    {
        fprintf(_errStream, "ov_read error %ld\n", amt);
        status = RSTAT_ERROR;
    }
    else
        status = amt ? 0 : RSTAT_EOF;

    buffer->used = count;
    if( count > 0 )
    {
        status |= RSTAT_DATA;
        st->sampleCount += count;
        REPORT_BUF("FAUN readOgg buf used: %d (%d)\n", count, st->sampleCount);
        //wav_write(wfp, buffer->sample.f32, count*2);
    }
    return status;
}


static int stream_fillBuffers(StreamOV*);

static void stream_start(StreamOV* st)
{
    FaunSource* src = _asource + st->sindex;
    FaunBuffer* buf = st->buffers;
    int i;

    if (! buf->sample.ptr)
    {
        // Allocate on first use; match attributes of voice mixing buffer.
        // Size each buffer to hold 1/4 second of data (multiple of 8 samples).

        int rate = _voice.mix.rate;
        int frameCount = ((rate / 4) + 7) & ~7;
        for (i = 0; i < STREAM_BUFFERS; ++i)
            faun_allocBufferSamples(buf + i, FAUN_F32, FAUN_CHAN_2,
                                    rate, frameCount);
    }

    faun_sourceResetQueue(src);

    for (i = 0; i < STREAM_BUFFERS; ++buf, ++i)
    {
        buf->used = 0;
        src->bufferQueue[i] = buf;
    }

    src->bufUsed = STREAM_BUFFERS;    // Prime faun_processedBuffer().
    st->feed = 1;

    stream_fillBuffers(st);

    if (st->sampleCount)
    {
        src->state = SS_PLAYING;
        src->playPos =
        src->framesOut = 0;
        REPORT_STREAM(st,"start");
    }
}


static void stream_stop(StreamOV* st)
{
    REPORT_STREAM(st,"stop");
    _asource[st->sindex].state = SS_STOPPED;
    st->feed = 0;

    if( st->chunk.cfile )
        stream_closeFile( st );
}


static void signalDone(const FaunSource* src)
{
    FaunSignal sig;

    sig.id = src->serialNo;
    sig.signal = FAUN_SIGNAL_DONE;
    //printf("signalDone %x\n", sig.id);

    tmsg_push(_voice.sig, &sig);

#ifdef CAPTURE
    if (endOnSignal)
        endCapture = 1;
#endif
}


/*
  Immediately set current volumes and halt fading.
*/
static inline void source_setGain(FaunSource* src, float volL, float volR)
{
    src->gainL = volL;
    src->gainR = volR;
    src->fadeL = src->fadeR = 0.0f;
}


#define FADE_DELTA(vol,period)  ((vol / period) / 44100.0f)
#define GAIN_SILENCE_THRESHOLD  0.001f

/*
  Set fadeL/fadeR to change current gains to target volumes over fadePeriod.
*/
static void source_setFadeDeltas(FaunSource* src)
{
    if (src->fadePeriod) {
        float inc = FADE_DELTA(1.0f, src->fadePeriod);
#if 1
        src->fadeL = inc * (src->targetL - src->gainL);
        src->fadeR = inc * (src->targetR - src->gainR);
#else
        src->fadeL = (src->targetL < src->gainL) ? -inc * src->gainL
                                                 :  inc * src->targetL;
        src->fadeR = (src->targetR < src->gainR) ? -inc * src->gainR
                                                 :  inc * src->targetR;
#endif
    } else {
        source_setGain(src, src->targetL, src->targetR);
    }

#if 0
    printf("KR fadeDeltas hz:%d per:%f %f,%f\n",
           _voice.updateHz, src->fadePeriod, src->fadeL, src->fadeR);
#endif
}


/*
  Set fadePos to (totalFrames - (fadePeriod * 44100)).
*/
static void source_initFadeOut(FaunSource* src, uint32_t totalFrames)
{
    uint32_t ff = (uint32_t) (src->fadePeriod * 44100.0f);
    // Avoiding overlap with any fade-in.
    if (totalFrames > 2*ff)
        src->fadePos = totalFrames - ff;
}


static inline void source_fadeOut(FaunSource* src)
{
    float inc = -FADE_DELTA(1.0f, src->fadePeriod);
    src->fadeL = inc * src->gainL;
    src->fadeR = inc * src->gainR;
    src->targetL = src->targetR = 0.0f;

    src->mode |= END_AFTER_FADE;
}


static void source_setMode(FaunSource* src, int mode)
{
    src->mode = mode;

    if (mode & FAUN_PLAY_FADE_IN) {
        src->gainL = src->gainR = 0.0f;
        src->targetL = src->targetR = src->playVolume;
        source_setFadeDeltas(src);
    } else if (mode & PLAY_TARGET_VOL) {
        // Reset after any previous fade out.
        source_setGain(src, src->targetL, src->targetR);
    } else {
        source_setGain(src, src->playVolume, src->playVolume);
    }
    src->endPos =
    src->fadePos = END_POS_NONE;
}


static void cmd_playSource(int si, uint32_t bufIds, int mode, uint32_t pid)
{
    FaunSource* src = _asource + si;
    FaunBuffer* buf = _abuffer + (bufIds & BID_PACKED);
    uint32_t ftotal;

    src->serialNo = pid;
    assert(si == (int) FAUN_PID_SOURCE(pid));

    //printf("CMD source play %d buf:%x mode:%x\n", si, bufIds, mode);
    faun_setBuffer(src, buf);
    ftotal = buf->used;
    for (bufIds >>= 10; bufIds; bufIds >>= 10) {
         buf = _abuffer + ((bufIds-1) & BID_PACKED);
        faun_queueBuffer(src, buf);
        ftotal += buf->used;
    }

    src->playPos = src->framesOut = 0;
    source_setMode(src, mode);

    if (mode & FAUN_PLAY_FADE_OUT)
        source_initFadeOut(src, ftotal);

    if (mode & (FAUN_PLAY_ONCE | FAUN_PLAY_LOOP))
        src->state = SS_PLAYING;
    else
        src->state = SS_STOPPED;
}


static void cmd_playStream(int si, const FileChunk* fc, int mode, uint32_t pid)
{
    StreamOV* st = _stream + (si - _sourceLimit);
    assert(si >= _sourceLimit);
    stream_stop( st );

    st->chunk = *fc;
    if (fc->offset)
        fseek(fc->cfile, fc->offset, SEEK_SET);

    if (ov_open_callbacks(&st->chunk, &st->vf, NULL, 0, chunkMethods) < 0)
    {
#ifdef GLV_ASSET_H
        glv_assetClose(&st->asset);
#else
        fclose(st->chunk.cfile);
        st->chunk.cfile = NULL;
#endif
        fprintf(_errStream, "Faun cannot open Ogg (stream %d)\n", si);
    }
    else
    {
        FaunSource* src = _asource + si;
        src->serialNo = pid;
        assert(si == (int) FAUN_PID_SOURCE(pid));

        st->feed = 0;
        st->sampleCount = 0;
        st->sampleLimit = 0;
        st->vinfo = ov_info(&st->vf, -1);

        source_setMode(src, mode);

        if (mode & FAUN_PLAY_FADE_OUT)
            source_initFadeOut(src, ov_pcm_total(&st->vf, -1));

        if (mode & (FAUN_PLAY_ONCE | FAUN_PLAY_LOOP))
            stream_start(st);
    }
}


static void cmd_playStreamPart(int si, double start, double duration, int mode)
{
    FaunSource* source = _asource + si;
    StreamOV* st = _stream + (si - _sourceLimit);
    assert(si >= _sourceLimit);

    st->feed = 0;
    st->start = start;
    st->sampleCount = 0;
    st->sampleLimit = (uint32_t) (duration * _voice.mix.rate);
    //printf("KR rate %d %d\n", st->vinfo->rate, st->sampleLimit);

    source_setMode(source, mode);

    source->state = SS_STOPPED;

    ov_time_seek(&st->vf, st->start);
    stream_start(st);
}


/**
  Decode audio from file until all available buffers are filled.
  Should only be called if st->feed and st->chunk.cfile are both non-zero.

  Return number of buffers filled with data.
*/
static int stream_fillBuffers(StreamOV* st)
{
    FaunSource* source = _asource + st->sindex;
    FaunBuffer* freeBuf;
    uint32_t excess;
    int fillCount = 0;
    int status;

    while ((freeBuf = faun_processedBuffer(source)))
    {
        REPORT_BUF("KR fillBuffer %ld\n", freeBuf - st->buffers);
        ++fillCount;
read_again:
        status = _readOgg(st, freeBuf);
        if( status & RSTAT_DATA )
        {
            if (SEGMENT_SET(st) && st->sampleCount >= st->sampleLimit)
            {
                status |= RSTAT_EOF;

                excess = st->sampleCount - st->sampleLimit;
                REPORT_BUF("    sampleCount: %d sampleLimit: %d\n",
                           st->sampleCount, st->sampleLimit);
                if (excess >= freeBuf->used)
                {
                    freeBuf->used = 0;
                    status &= ~RSTAT_DATA;
                    goto drop_buf;
                }
                freeBuf->used -= excess;
            }
            faun_queueBuffer(source, freeBuf);
drop_buf:
            REPORT_BUF("    used: %d\n", freeBuf->used);
        }

        if( status & RSTAT_ERROR )
        {
            stream_closeFile( st );
            break;
        }
        else if( status & RSTAT_EOF )
        {
            REPORT_BUF("    end-of-stream\n");
            if (source->mode & FAUN_PLAY_LOOP)
            {
                if (SEGMENT_SET(st))
                {
                    ov_time_seek(&st->vf, st->start);
                    st->sampleCount = 0;
                }
                else
                    ov_raw_seek(&st->vf, 0);

                // If the stream ended exactly on a buffer boundary then the
                // unqueued buffer is still available.
                if (! (status & RSTAT_DATA))
                    goto read_again;
            }
            else if (SEGMENT_SET(st))
            {
                // Allow the current buffers to finish playing, but stop
                // feeding more data.
                st->feed = 0;
                break;
            }
            else
            {
                stream_closeFile( st );
                break;
            }
        }
    }

    return fillCount;
}

//----------------------------------------------------------------------------

#define SIMUL_MIX

static int faun_fadeChan(float* fadeTriplet, int fading, int fadeMask)
{
    float gain   = fadeTriplet[0];
    float fade   = fadeTriplet[2];
    float target = fadeTriplet[4];

    gain += fade;
    if (fade < 0.0f) {
        if (gain <= target)
            goto done;
    } else if (gain >= target) {
        goto done;
    }
    fadeTriplet[0] = gain;
    return fading;

done:
    fadeTriplet[0] = target;
    fadeTriplet[2] = 0.0f;
    return fading & fadeMask;
}

static void _mix1StereoFade(float* restrict output, float* end,
                            const float* restrict input, FaunSource* src)
{
    const int FADE_L = 1;
    const int FADE_R = 2;
    int fading = src->fadeL ? FADE_L : 0;
    if (src->fadeR)
        fading |= FADE_R;
    assert(fading);

    while (output != end) {
        *output += *input++ * src->gainL;
        output++;
        *output += *input++ * src->gainR;
        output++;

        if (fading & FADE_L)
            fading = faun_fadeChan(&src->gainL, fading, ~FADE_L);
        if (fading & FADE_R)
            fading = faun_fadeChan(&src->gainR, fading, ~FADE_R);
        if (! fading) {
            if (src->mode & END_AFTER_FADE) {
                src->endPos = src->framesOut;   // Force end of play.
            } else {
                while (output != end) {
                    *output += *input++ * src->gainL;
                    output++;
                    *output += *input++ * src->gainR;
                    output++;
                }
            }
            break;
        }
    }
}

void faun_fadeBuffers(float* output, const float** inputEnd,
                      FaunSource** src, int inCount, uint32_t sampleCount)
{
    float* end = output + sampleCount;
    int i;
    for (i = 0; i < inCount; ++i) {
        --inputEnd;
        _mix1StereoFade(output, end, *inputEnd, src[i]);
    }
}


#ifdef SIMUL_MIX
static void _mix1Stereo(float* restrict output, float* end,
                        const float* restrict input,
                        float gainL, float gainR, int init)
{
    if (init) {
        while (output != end) {
            *output++ = *input++ * gainL;
            *output++ = *input++ * gainR;
        }
    } else {
        while (output != end) {
            *output += *input++ * gainL;
            output++;
            *output += *input++ * gainR;
            output++;
        }
    }
}

static void _mix2Stereo(float* restrict output, float* end,
                        const float** input,
                        const float* gainL, const float* gainR, int init)
{
    const float* restrict in0 = input[0];
    const float* restrict in1 = input[1];
    if (init) {
        while (output != end) {
            *output++ = (*in0++ * gainL[0]) + (*in1++ * gainL[1]);
            *output++ = (*in0++ * gainR[0]) + (*in1++ * gainR[1]);
        }
    } else {
        while (output != end) {
            *output += (*in0++ * gainL[0]) + (*in1++ * gainL[1]);
            output++;
            *output += (*in0++ * gainR[0]) + (*in1++ * gainR[1]);
            output++;
        }
    }
}

static void _mix4Stereo(float* restrict output, float* end,
                        const float** input,
                        const float* gainL, const float* gainR, int init)
{
    const float* restrict in0 = input[0];
    const float* restrict in1 = input[1];
    const float* restrict in2 = input[2];
    const float* restrict in3 = input[3];
    if (init) {
        while (output != end) {
            *output++ = (*in0++ * gainL[0]) + (*in1++ * gainL[1]) +
                        (*in2++ * gainL[2]) + (*in3++ * gainL[3]);
            *output++ = (*in0++ * gainR[0]) + (*in1++ * gainR[1]) +
                        (*in2++ * gainR[2]) + (*in3++ * gainR[3]);
        }
    } else {
        while (output != end) {
            *output += (*in0++ * gainL[0]) + (*in1++ * gainL[1]) +
                       (*in2++ * gainL[2]) + (*in3++ * gainL[3]);
            output++;
            *output += (*in0++ * gainR[0]) + (*in1++ * gainR[1]) +
                       (*in2++ * gainR[2]) + (*in3++ * gainR[3]);
            output++;
        }
    }
}
#endif


/*
 * Mix stereo inputs.
 *
 * \param output        Output buffer for the mixed samples.
 * \param input         Array of pointers to float input buffers.
 * \param gainL         Left channel gain for each input.
 * \param gainR         Right channel gain for each input.
 * \param inCount       Number of inputs.
 * \param sampleCount   Number of samples (frames*2) to mix from each input.
 */
void faun_mixBuffers(float* output, const float** input,
                     const float* gainL, const float* gainR,
                     int inCount, uint32_t sampleCount)
{
#ifdef SIMUL_MIX
    float* end = output + sampleCount;
    int initial = 1;

    while (inCount > 3) {
        inCount -= 4;
        _mix4Stereo(output, end, input, gainL, gainR, initial);
        initial = 0;
        input += 4;
        gainL += 4;
        gainR += 4;
    }

    switch (inCount) {
        case 3:
            _mix2Stereo(output, end, input, gainL, gainR, initial);
            _mix1Stereo(output, end, input[2], gainL[2], gainR[2], 0);
            break;
        case 2:
            _mix2Stereo(output, end, input, gainL, gainR, initial);
            break;
        case 1:
            _mix1Stereo(output, end, input[0], gainL[0], gainR[0], initial);
            break;
        default:
            if (initial)
                memset(output, 0, sampleCount * sizeof(float));
            break;
    }
#else
    float* out;
    float* end;
    const float* in;
    float gainL, gainR;
    int i;
    int mixed = 0;

    if (inCount > 0) {
        end = output + sampleCount;

        gainL = inGainL[0];
        gainR = inGainR[0];
        if (gainL > GAIN_SILENCE_THRESHOLD ||
            gainR > GAIN_SILENCE_THRESHOLD) {
            in = input[0];
            out = output;
            while (out != end) {
                *out++ = *in++ * gainL;
                *out++ = *in++ * gainR;
            }
            ++mixed;
        }

        for (i = 1; i < inCount; i++) {
            gainL = inGainL[i];
            gainR = inGainR[i];
            if (gainL > GAIN_SILENCE_THRESHOLD ||
                gainR > GAIN_SILENCE_THRESHOLD) {
                in = input[i];
                out = output;
                while (out != end) {
                    *out += *in++ * gainL;
                    out++;
                    *out += *in++ * gainR;
                    out++;
                }
                ++mixed;
            }
        }
    }

    if (! mixed)
        memset(output, 0, sampleCount * sizeof(float));
#endif
}

//----------------------------------------------------------------------------

static void faun_evalProg(FaunProgram* prog, uint32_t mixClock)
{
    const uint8_t* pc;
    const uint8_t* end;
    FaunSource* src;

    if (prog->waitPos) {
        if (mixClock < prog->waitPos)
            return;
        prog->waitPos = 0;
    }

    pc  = prog->code + prog->pc;
    end = prog->code + prog->used;

    do {
        switch (*pc++) {
            default:
                fprintf(_errStream, "Invalid opcode %x\n", pc[-1]);
                // Fall through...

            case FO_END:
                prog->pc = prog->used = 0;
                prog->running = 0;
                return;

            case FO_WAIT:
                prog->waitPos = mixClock + (*pc++) * 4410;
                prog->pc = pc - prog->code;
                return;

            case FO_SOURCE:
                prog->si = *pc++;
                break;

            case FO_QUEUE:
                if (prog->si < _sourceLimit)
                    faun_queueBuffer(_asource + prog->si, _abuffer + (*pc++));
                break;

            case FO_PLAY_BUF:
                if (prog->si < _sourceLimit)
                    cmd_playSource(prog->si, pc[0], pc[1], prog->si);
                pc += 2;
                break;

            case FO_START_STREAM:
                if (prog->si >= _sourceLimit) {
                    source_setMode(_asource + prog->si, pc[0]);
                    stream_start(_stream + (prog->si - _sourceLimit));
                }
                ++pc;
                break;

            case FO_SET_VOL:
                // Set playVolume parameter; current volume is not changed.
                _asource[prog->si].playVolume = (float) (*pc++) / 255.0f;
                break;

            case FO_SET_FADE:
                _asource[prog->si].fadePeriod = (float) (*pc++) / 10.0f;
                break;

            case FO_SET_END:
            {
                uint32_t pos = *pc++;
                _asource[prog->si].endPos = pos ? pos * 4410 : END_POS_NONE;
            }
                break;

            case FO_LOOP_ON:
            case FO_LOOP_OFF:
                src = _asource + prog->si;
            {
                uint16_t mode = src->mode & ~(FAUN_PLAY_ONCE | FAUN_PLAY_LOOP);
                if (pc[-1] == FO_LOOP_ON)
                    mode |= FAUN_PLAY_LOOP;
                src->mode = mode;
            }
                break;

            /*
            case FO_FADE_ON:
            case FO_FADE_OFF:
                src = _asource + prog->si;
            {
                uint16_t mode = src->mode & ~FAUN_PLAY_FADE;
                if (pc[-1] == FO_FADE_ON)
                    mode |= FAUN_PLAY_FADE;
                src->mode = mode;
            }
                break;
            */

            case FO_FADE_IN:
                src = _asource + prog->si;
                src->gainL = src->gainR = 0.0f;
                src->targetL = src->targetR = src->playVolume;
                source_setFadeDeltas(src);
                break;

            case FO_FADE_OUT:
                src = _asource + prog->si;
                source_fadeOut(src);
                break;

            case FO_VOL_LR:     // L volume, R volume
                source_setGain(_asource + prog->si,
                               (float) pc[0] / 255.0f,
                               (float) pc[1] / 255.0f);
                pc += 2;
                break;

            case FO_PAN:        // L target, R target
                // Fade current volumes to target volumes over fadePeriod.
                src = _asource + prog->si;
                src->targetL = (float) pc[0] / 255.0f;
                src->targetR = (float) pc[1] / 255.0f;
                source_setFadeDeltas(src);
                pc += 2;
                break;

            case FO_SIGNAL:
            {
                FaunSignal sig;
                sig.id = prog->si;
                sig.signal = FAUN_SIGNAL_PROG;
                tmsg_push(_voice.sig, &sig);
            }
                break;

            case FO_CAPTURE:
#ifdef CAPTURE
            {
                char* outfile = getenv("FAUN_CAPTURE");
                if (outfile && wfp == NULL) {
                    wfp = wav_open(outfile, 44100 /*voice->mix.rate*/, 16, 2);
                    endOnSignal = endCapture = 0;
                }
            }
#endif
                break;
#if 0
            case FO_SET_VOL_f:
            {
                const float* farg = (float*) prog->code;
                _asource[prog->si].playVolume = farg[ *pc++ ];
            }
                break;

            case FO_SET_FADE_f:
            {
                const float* farg = (float*) prog->code;
                _asource[prog->si].fadePeriod = farg[ *pc++ ];
            }
                break;

            case FO_VOL_LR_f:
            {
                const float* farg = (float*) prog->code;
                source_setGain(_asource + prog->si, farg[pc[0]], farg[pc[1]]);
            }
                pc += 2;
                break;

            case FO_PAN_f:
            {
                const float* farg = (float*) prog->code;
                src = _asource + prog->si;
                src->targetL = farg[pc[0]];
                src->targetR = farg[pc[1]];
                source_setFadeDeltas(src);
            }
                pc += 2;
                break;
#endif
        }
    } while (pc != end);

    prog->pc = prog->used = 0;
}

//----------------------------------------------------------------------------

//#include "cpuCounter.h"

#ifdef _WIN32
static DWORD WINAPI audioThread(LPVOID arg)
#else
static void* audioThread(void* arg)
#endif
{
    FaunVoice* voice = arg;
    FaunSource* src;
    FaunBuffer* buf;
    FaunProgram* prog;
    StreamOV* st;
    FaunSource** mixSource;
    FaunSource** fadeSource;
    const float** input;
    float* inputGainL;
    float* inputGainR;
    const char* error;
    char cmdBuf[sizeof(CommandF) * 2];
    CommandA* cmd = (CommandA*) cmdBuf;
    int sourceCount;
    uint32_t mixed;
    uint32_t totalMixed = 0;    // Wraps after 27 hours.
    uint32_t fragmentLen;
    uint32_t samplesAvail;
    uint32_t mixSampleLen = voice->mix.used;
    uint32_t fileChunkSizeArg = 0;
    int i;
    struct MsgPort* port = voice->cmd;
    MsgTime ts;
    int updateMs = 1000/voice->updateHz - 2;
    int sleepTime = updateMs;
    int scount;
    int n, fn;

#ifdef CPUCOUNTER_H
    uint64_t t0, tp, tc, tm, tw;
#define COUNTER(V)  V = cpuCounter()
#else
#define COUNTER(V)
#endif

#ifdef _WIN32
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
#endif

    n = scount = _sourceLimit + _streamLimit;
    mixSource  = (FaunSource**) malloc(n * sizeof(void*) * 5);
    fadeSource = mixSource + n;
    input      = (const float**) (fadeSource + n);
    inputGainL = (float*) (input + n);
    inputGainR = inputGainL + n;

    tmsg_setTimespec(&ts, sleepTime);

    for (;;)
    {
        // Wait for commands.
        COUNTER(t0);
        if (sleepTime > 0)
        {
            n = tmsg_popTimespec(port, cmd, &ts);
        }
        else
            n = tmsg_pop(port, cmd);
#ifdef CPUCOUNTER_H
        printf("CT msg   %9ld\n", cpuCounter() - t0);
#endif
        if( n < 0 )
        {
            fprintf(_errStream, "audioThread message port error\n");
            break;
        }
        else if( n == 0 )
        {
            switch (cmd->op) {
                case CMD_QUIT:
                    goto cleanup;

                case CMD_SUSPEND:
                    sleepTime = -1;
                    sysaudio_stopVoice(voice);
                    break;

                case CMD_RESUME:
                    sleepTime = updateMs;
                    sysaudio_startVoice(voice);
                    break;

                case CMD_PROGRAM:
                    prog = _pexec + cmdBuf[2];
                    prog->used = 0;
                    prog->running = 1;
                    goto read_prog;

                case CMD_PROGRAM_END:
                    prog = _pexec + cmdBuf[2];
                    prog->running = 1;
                    goto read_prog;

                case CMD_PROGRAM_MID:
                    prog = _pexec + cmdBuf[2];
read_prog:
                    n = cmd->select;
                    //printf("CMD prog %d %d len:%d\n", cmdBuf[2], cmd->op, n);
                    if (prog->used + n > FAUN_PROGRAM_MAX) {
                        prog->running = 0;
                        fprintf(_errStream, "Program buffer overflow\n");
                    } else {
                        memcpy(prog->code + prog->used, cmdBuf + PROG_CHEAD, n);
                        prog->used += n;
                    }
                    break;

                case CMD_PROGRAM_BEG:
                    prog = _pexec + cmdBuf[2];
                    prog->used = 0;
                    prog->running = 0;
                    goto read_prog;

                case CMD_SET_BUFFER:
                    //printf("CMD set buffer bi:%d cmdPart:%d\n",
                    //       cmd->select, cmdPart);
                    buf = _abuffer + cmd->select;
                    free(buf->sample.ptr);
                    faun_detachBuffers();

                    // Command contains sample, avail, & used members.
                    memcpy(buf, cmdBuf + 2, 16);

                    // The other parameters match _allocBufferVoice().
                    buf->rate = voice->mix.rate;
                    buf->format = FAUN_F32;
                    buf->chanLayout = FAUN_CHAN_2;
                    break;

                case CMD_BUFFERS_FREE:
                    faun_freeBufferSamples(cmd->ext, _abuffer + cmd->select);
                    faun_detachBuffers();
                    break;

                case CMD_PLAY_SOURCE:
                    src = _asource + cmd->select;
                    cmd_playSource(cmd->select, cmd->arg.u32[0], cmd->ext,
                                   cmd->arg.u32[1]);
                    break;

                case CMD_PLAY_SOURCE_VOL:
                    src = _asource + cmd->select;
                    src->targetL = cmd->arg.f[2];
                    src->targetR = cmd->arg.f[3];
                    cmd_playSource(cmd->select, cmd->arg.u32[0],
                                   cmd->ext | PLAY_TARGET_VOL,
                                   cmd->arg.u32[1]);
                    break;

                case CMD_OPEN_STREAM_SIZE:
                    // Save FileChunk size for the immediately following
                    // CMD_OPEN_STREAM.
                    fileChunkSizeArg = cmd->arg.u32[1];
                    break;

                case CMD_OPEN_STREAM:
                {
                    FileChunk fc;
                    fc.offset = cmd->arg.u32[1];
                    memcpy(&fc.cfile, &cmd->arg.u32[2], sizeof(void*));
#if __SIZEOF_POINTER__ == 4
                    fc.size = cmd->arg.u32[3];
#else
                    fc.size = fileChunkSizeArg;
                    fileChunkSizeArg = 0;
#endif
                    //printf("CMD open stream %d %d %d\n",
                    //       cmd->select, fc.offset, cmd->ext);
                    cmd_playStream(cmd->select, &fc, cmd->ext, cmd->arg.u32[0]);
                }
                    break;

                case CMD_PLAY_STREAM_PART:
                {
                    double d[2];
                    memcpy(d, cmdBuf+4, sizeof(double)*2);
                    //printf("CMD stream part %d %f %f\n",
                    //       cmd->select, d[0], d[1]);
                    cmd_playStreamPart(cmd->select, d[0], d[1], cmd->ext);
                }
                    break;

                case CMD_VOLUME_VARY:
                    src = _asource + cmd->select;
                    src->targetL    = cmd->arg.f[0];
                    src->targetR    = cmd->arg.f[1];
                    src->fadePeriod = cmd->arg.f[2];
                    source_setFadeDeltas(src);
                    break;

                case CMD_CON_START:
                case CMD_CON_STOP:
                case CMD_CON_RESUME:
                    i = cmd->select;
                    n = i + cmd->ext;
                    for ( ; i < n; ++i) {
                        src = _asource + i;
                        if (src->qactive != QACTIVE_NONE) {
                            src->state = (cmd->op == CMD_CON_STOP) ?
                                            SS_STOPPED : SS_PLAYING;
                        }
                    }
                    break;

                case CMD_CON_FADE_OUT:
                    //printf("CMD control %x %d\n", cmd->select,
                    //        cmd->op - CMD_CON_START);
                    i = cmd->select;
                    n = i + cmd->ext;
                    for ( ; i < n; ++i) {
                        src = _asource + i;
                        source_fadeOut(src);
                    }
                    break;

                case CMD_PARAM_VOLUME:
                    //printf("CMD param-vol %d:%d %f\n",
                    //       cmd->select, cmd->ext, cmd->arg.f[0]);
                    src = _asource + cmd->select;
                    n = cmd->ext;
                    while (n--) {
                        src->playVolume = cmd->arg.f[0];
                        ++src;
                    }
                    break;

                case CMD_PARAM_FADE_PERIOD:
                    src = _asource + cmd->select;
                    n = cmd->ext;
                    while (n--) {
                        src->fadePeriod = cmd->arg.f[0];
                        ++src;
                    }
                    break;

                case CMD_PARAM_END_TIME:
                    src = _asource + cmd->select;
                    if (cmd->arg.f[0] <= 0.01f)
                        src->endPos = END_POS_NONE;
                    else
                        src->endPos = (uint32_t) (44100.0f * cmd->arg.f[0]);
                    break;
            }
            continue;
        }

        // Go back to waiting if suspended.
        if (sleepTime < 0)
            continue;

        for (i = 0; i < _pexecLimit; ++i)
        {
            prog = _pexec + i;
            if (prog->running)
                faun_evalProg(prog, totalMixed);
        }

        COUNTER(tp);

        // Collect active sources.
        sourceCount = 0;
        for (i = 0; i < _sourceLimit; ++i)
        {
            src = _asource + i;
            if (src->state == SS_PLAYING)
            {
                //if (src->fadeL || src->fadeR)
                //    source_fade(src, NULL);
                if (src->qactive != QACTIVE_NONE)
                    mixSource[sourceCount++] = src;
            }
        }

        // Read streams and collect their sources.
        n = 0;
        for (i = 0; i < _streamLimit; ++i)
        {
            st = _stream + i;
            src = _asource + st->sindex;
            if (src->state == SS_PLAYING)
            {
                //if (src->fadeL || src->fadeR)
                //    source_fade(src, st);
                if (st->feed && st->chunk.cfile) {
                    // Decoding only one stream per loop unless some streams
                    // have no previously filled buffer to play.

                    if (n == 0 || src->qactive == QACTIVE_NONE)
                        n += stream_fillBuffers(st);
                }
                if (src->qactive != QACTIVE_NONE)
                    mixSource[sourceCount++] = src;
            }
        }
        //printf("KR sbuf %d\n", n);

        COUNTER(tc);

        // Mix active sources into voice buffer.
        for (mixed = 0; mixed < mixSampleLen; )
        {
            // Determine size of fragment for this mix pass.
            fragmentLen = mixSampleLen - mixed;
            n = fn = 0;
            for (i = 0; i < sourceCount; ++i)
            {
                src = mixSource[i];
                if (src->qactive != QACTIVE_NONE)
                {
                    buf = src->bufferQueue[src->qactive];
                    if (src->fadeL || src->fadeR) {
                        fadeSource[fn++] = src;
                        input[scount - fn] = buf->sample.f32 + src->playPos*2;
                    } else {
                        input[n] = buf->sample.f32 + src->playPos*2;
                        inputGainL[n] = src->gainL;
                        inputGainR[n] = src->gainR;
                        ++n;
                    }

                    samplesAvail = buf->used - src->playPos;
                    if (samplesAvail < fragmentLen)
                        fragmentLen = samplesAvail;
                }

                REPORT_MIX("     mix source %d qactive:%d pos:%d\n",
                           i, src->qactive, src->playPos);
            }

            // Mix fragment.
            REPORT_MIX("FAUN mixBuffers count:%d mixed:%4d/%d frag:%4d\n",
                       sourceCount, mixed, mixSampleLen, fragmentLen);
            {
            float* voiceSamples = voice->mix.sample.f32 + mixed*2;
            faun_mixBuffers(voiceSamples, input,
                            inputGainL, inputGainR, n, fragmentLen*2);
            if (fn) {
                faun_fadeBuffers(voiceSamples, input + scount,
                                 fadeSource, fn, fragmentLen*2);
            }
            }

            // Advance play positions.
            for (i = 0; i < sourceCount; ++i)
            {
                src = mixSource[i];
                if (src->qactive != QACTIVE_NONE)
                {
                    uint32_t pos = src->framesOut + fragmentLen;

                    src->framesOut = pos;
                    if (pos >= src->endPos)
                    {
end_play:
                        faun_deactivate(src, SOURCE_ID(src));
                        if (src->mode & FAUN_SIGNAL_DONE)
                            signalDone(src);
                    }
                    else
                    {
                        if (pos >= src->fadePos)
                            source_fadeOut(src);

                        pos = src->playPos + fragmentLen;
                        buf = src->bufferQueue[src->qactive];
                        if (pos >= buf->used)
                        {
                            // Load next buffer.
                            src->playPos = 0;
                            n = src->qactive;
                            if (++n == SOURCE_QUEUE_SIZE)
                                n = 0;
                            if (n == src->qtail) {
                                //printf("FAUN tail %d\n", n);
                                if ((src->mode & FAUN_PLAY_LOOP) &&
                                    ((int) SOURCE_ID(src) < _sourceLimit))
                                    continue;
                                goto end_play;
                            } else {
                                // Abort if a buffer was freed.
                                if (! src->bufferQueue[n]->sample.ptr)
                                    goto end_play;
                                src->qactive = n;
                            }
                        }
                        else
                            src->playPos = pos;
                    }
                }
            }
            mixed += fragmentLen;
            totalMixed += fragmentLen;
        }

        // Send final mix to audio system.

        COUNTER(tm);
        error = sysaudio_write(voice, voice->mix.sample.f32,
                               mixed*2 * sizeof(float));
#ifdef CPUCOUNTER_H
        COUNTER(tw);
        printf("CT col   %9ld\n"
               "CT mix   %9ld\n"
               "CT write %9ld\n", tc - tp, tm - tc, tw - tm);
#endif
        if (error)
            fprintf(_errStream, "Faun sysaudio_write: %s\n", error);

#ifdef CAPTURE
        if (wfp) {
            wav_write(wfp, voice->mix.sample.f32, mixed*2);
            if (endCapture) {
                wav_close(wfp);
                wfp = NULL;
            }
        }
#endif

        tmsg_setTimespec(&ts, sleepTime);
    }

cleanup:
#ifdef CAPTURE
    if (wfp) {
        wav_close(wfp);
        wfp = NULL;
    }
#endif
    free(mixSource);
#ifdef _WIN32
    CoUninitialize();
#endif
    return 0;
}


// Private testing function.
void faun_closeOnSignal()
{
#ifdef CAPTURE
    endOnSignal = 1;
#endif
}


#define faun_command(buf,len)   tmsg_push(_voice.cmd, buf)


static void faun_command2(int op, int select)
{
    char buf[MSG_SIZE];
    buf[0] = op;
    buf[1] = select;
    faun_command(buf, 2);
}


//----------------------------------------------------------------------------

/**
  \example basic.c
  This is a basic example of how to use the API.

  \file faun.h
  The Faun programmer interface.

  \def FAUN_VERSION_STR
  A printable string of the library version.

  \def FAUN_VERSION
  Three packed bytes containing the major, minor, & fix version numbers.

  \def FAUN_PROGRAM_MAX
  The maximum number of bytes for the faun_program() length.

  \def FAUN_PAIR(a,b)
  Used with faun_playSource() to queue two buffers that will be played
  sequentially.

  \def FAUN_TRIO(a,b,c)
  Used with faun_playSource() to queue three buffers that will be played
  sequentially.

  \def FAUN_PID_SOURCE(pid)
  Get the source index from a playback identifier.


  \enum FaunCommand
  Commands used for faun_control().

  \var FaunCommand::FC_START
  Start playing from the beginning of the source buffer or stream.

  \var FaunCommand::FC_STOP
  Halt playback immediately.

  \var FaunCommand::FC_RESUME
  Continue playback from the point when #FC_STOP was last used.

  \var FaunCommand::FC_FADE_OUT
  Fade volume to zero from the current play position over the
  #FAUN_FADE_PERIOD.


  \enum FaunPlayMode
  Playback mode options for faun_playSource(), faun_playStream(), &
  faun_playStreamPart().

  \var FaunPlayMode::FAUN_PLAY_ONCE
  Used to initiate playback of a source or stream a single time.

  \var FaunPlayMode::FAUN_PLAY_LOOP
  Used to initiate playback of a source or stream and repeat it forever.

  \var FaunPlayMode::FAUN_PLAY_FADE_IN
  Increase gain from 0.0 gradually when playing begins.
  The target gain is set by #FAUN_VOLUME and the fade period is set by
  #FAUN_FADE_PERIOD.

  \var FaunPlayMode::FAUN_PLAY_FADE_OUT
  Decreases gain to 0.0 gradually just before the source or stream ends.
  The fade period is set by #FAUN_FADE_PERIOD.

  \var FaunPlayMode::FAUN_PLAY_FADE
  Used to set both #FAUN_PLAY_FADE_IN & #FAUN_PLAY_FADE_OUT.

  \var FaunPlayMode::FAUN_SIGNAL_DONE
  Used to generate a FaunSignal when the source or stream is finished playing.

  \var FaunPlayMode::FAUN_SIGNAL_PROG
  The FaunSignal::signal identifier of a signal generated by the FO_SIGNAL
  program opcode.


  \enum FaunParameter
  Parameters are used to modify playback of a source or stream.
  They are modified using faun_setParameter().

  \var FaunParameter::FAUN_VOLUME
  This is the volume (or fade in target) used when playback begins.
  The value ranges from 0.0 to 1.0.  The default value is 1.0.

  \var FaunParameter::FAUN_FADE_PERIOD
  Duration in seconds for fading in & out.  The default value is 1.5 seconds.

  \var FaunParameter::FAUN_END_TIME
  Used to end playback of a source or stream before the buffer or stream file
  ends.  The value is the number of seconds from the start when the sound will
  be stopped.


  \struct FaunSignal
  This struct is used for faun_pollSignals() & faun_waitSignal().

  \var FaunSignal::id
  This is the playback identifier of the source generating the signal.

  For program signals generated by FO_SIGNAL this is the most recently
  selected source index (via FO_SOURCE).

  \var FaunSignal::signal
  This is the FaunPlayMode event (#FAUN_SIGNAL_DONE, #FAUN_SIGNAL_PROG)
  which occurred.
*/


static int limitU(int val, int max)
{
    if (val > max)
        val = max;
    else if (val < 0)
        val = 0;
    return val;
}


/**
  Called once at program startup.

  Stream identifier numbers start at the source limit.  So if sourceLimit is
  8 and streamLimit is 2, then the valid stream ids will be 8 & 9.

  \param bufferLimit    Maximum number of buffers (0-256).
  \param sourceLimit    Maximum number of simultaneously playing sounds (0-32).
  \param streamLimit    Maximum number of simultaneously playing streams (0-6).
  \param progLimit      Maximum number of program execution units (0-16).
  \param appName        Program identifier for networked audio systems.

  \returns Error string or `NULL` if successful.
*/
const char* faun_startup(int bufferLimit, int sourceLimit, int streamLimit,
                         int progLimit, const char* appName)
{
    const int DEF_UPDATE_HZ = 48;
    const char* error;
    int i, siLimit;

    if (! appName)
        appName = "Faun Audio";

    _errStream = stderr;
    if ((error = sysaudio_open(appName)))
        return error;

    _bufferLimit = bufferLimit = limitU(bufferLimit, BUFFER_MAX);
    _sourceLimit = sourceLimit = limitU(sourceLimit, SOURCE_MAX);
    _streamLimit = streamLimit = limitU(streamLimit, STREAM_MAX);
    _pexecLimit  = progLimit   = limitU(progLimit,   PEXEC_MAX);

    siLimit = _sourceLimit + _streamLimit;

#if 0
    printf("FaunBuffer:%ld FaunSource:%ld StreamOV:%ld FaunProgram:%ld\n",
           sizeof(FaunBuffer), sizeof(FaunSource), sizeof(StreamOV),
           sizeof(FaunProgram));
    // FaunBuffer:24 FaunSource:88 StreamOV:1176 FaunProgram:80
#endif
    assert(sizeof(FaunBuffer) % 8 == 0);
    assert(sizeof(FaunSource) % 8 == 0);
    assert(sizeof(StreamOV) % 8 == 0);
    assert(sizeof(FaunProgram) % 8 == 0);

    i = bufferLimit * sizeof(FaunBuffer) +
        siLimit     * sizeof(FaunSource) +
        streamLimit * sizeof(StreamOV) +
        progLimit   * sizeof(FaunProgram) +
        siLimit     * sizeof(_Atomic uint32_t);
    _abuffer = (FaunBuffer*) malloc(i);
    if (! _abuffer) {
        sysaudio_close();
        return "No memory for arrays";
    }

    _asource = (FaunSource*) (_abuffer + bufferLimit);
    _stream  = (StreamOV*)  (_asource + siLimit);

    memset(_abuffer, 0, bufferLimit * sizeof(FaunBuffer));
    for (i = 0; i < siLimit; ++i)
        faun_sourceInit(_asource + i, i);
    for (i = 0; i < streamLimit; ++i)
        stream_init(_stream + i, sourceLimit + i);

    if (progLimit) {
        _pexec = (FaunProgram*) (_stream + _streamLimit);
        memset(_pexec, 0, progLimit * sizeof(FaunProgram));
        _playbackId = (_Atomic uint32_t*) (_pexec + progLimit);
    } else {
        _pexec = NULL;
        _playbackId = (_Atomic uint32_t*) (_stream + _streamLimit);
    }

    for (i = 0; i < siLimit; ++i)
        atomic_init(_playbackId + i, NUL_PLAY_ID);
    _playSerialNo = NUL_PLAY_ID;
    atomic_flag_clear(&_pidLock);

    faun_allocBufferSamples(&_voice.mix, FAUN_F32, FAUN_CHAN_2, 44100,
                            44100 / DEF_UPDATE_HZ);

    // Set defaults which sysaudio_allocVoice may change.
    _voice.mix.used = _voice.mix.avail;
    _voice.updateHz = DEF_UPDATE_HZ;

    if ((error = sysaudio_allocVoice(&_voice, DEF_UPDATE_HZ, appName))) {
        sysaudio_close();
        return error;
    }

    _audioUp = AUDIO_UP;


    // Start audioThread.

    if (! (_voice.cmd = tmsg_create(MSG_SIZE, 32))) {
        error = "Command port create failed";
        goto thread_fail0;
    }

    if (! (_voice.sig = tmsg_create(sizeof(FaunSignal), 32))) {
        error = "Signal port create failed";
        goto thread_fail1;
    }

    if (threadCreateF(_voice.thread, audioThread, &_voice)) {
        error = "Voice thread create failed";
        goto thread_fail2;
    }

    _audioUp = AUDIO_THREAD_UP;
    return NULL;

thread_fail2:
    tmsg_destroy(_voice.sig);
    _voice.sig = NULL;
thread_fail1:
    tmsg_destroy(_voice.cmd);
    _voice.cmd = NULL;
thread_fail0:
    faun_shutdown();
    return error;
}


/**
  Called once when the program exits.
  It is safe to call this even if faun_startup() was not called.
*/
void faun_shutdown()
{
    StreamOV* st;
    int i;

    if (_audioUp == AUDIO_THREAD_UP) {
        faun_command2(CMD_QUIT, 0);
        threadJoin(_voice.thread);

        tmsg_destroy(_voice.cmd);
        tmsg_destroy(_voice.sig);
    }

    if (_audioUp) {
        for (i = 0; i < _streamLimit; ++i) {
            st = _stream + i;
            stream_stop(st);
            stream_free(st);
        }

        faun_freeBufferSamples(_bufferLimit, _abuffer);
        faun_freeBufferSamples(1, &_voice.mix);

        free(_abuffer);
        _abuffer = NULL;
        _asource = NULL;
        _stream  = NULL;

        sysaudio_freeVoice(&_voice);
        sysaudio_close();
        _audioUp = AUDIO_DOWN;
    }
}


/**
  Pause or resume mixing.

  \param halt  Non-zero suspends and zero resumes.
*/
void faun_suspend(int halt)
{
    if (_audioUp)
        faun_command2(halt ? CMD_SUSPEND : CMD_RESUME, 0);
}


/**
  Redirect error messages from _stderr_.  Pass `NULL` to reset to _stderr_.
*/
void faun_setErrorStream(FILE* fp)
{
    _errStream = fp ? fp : stderr;
}


/**
  Check for signals from sources and streams.

  \param sigbuf    Pointer to memory for signals.
  \param count     Number of signals sigbuf can hold.

  \return Number of signals copied to sigbuf.
*/
int faun_pollSignals(FaunSignal* sigbuf, int count)
{
    if( _audioUp )
    {
        int n = tmsg_used(_voice.sig);
        if (n > 0) {
            if (n > count)
                n = count;
            else
                count = n;
            while (n) {
                tmsg_pop(_voice.sig, sigbuf);
                ++sigbuf;
                --n;
            }
            return count;
        }
    }
    return 0;
}


/**
  Block calling thread until a signal is emitted.

  \param sigbuf     Memory for next signal.
*/
void faun_waitSignal(FaunSignal* sigbuf)
{
    if( _audioUp )
        tmsg_pop(_voice.sig, sigbuf);
}


/**
  Send a single command to sources or streams.

  \param si         Source or stream index.
  \param count      Number of sources or streams to command.
  \param command    FaunCommand enum.
*/
void faun_control(int si, int count, int command)
{
    if( _audioUp && command < FC_COUNT)
    {
        CommandA cmd;
        cmd.op     = command + CMD_CON_START;
        cmd.select = si;
        cmd.ext    = count;
        faun_command(&cmd, 4);
    }
}


#if 0
/*
  Send a number of commands to sources or streams.
*/
void faun_controlSeq(int si, int count, const uint8_t* commands)
{
}
#endif


/**
  Set source or stream parameter.

  \param si     Source or stream index.
  \param count  Number of sources or streams to modify.
  \param param  FaunParameter enum
                (#FAUN_VOLUME, #FAUN_FADE_PERIOD, #FAUN_END_TIME).
  \param value  Value assigned to param.
*/
void faun_setParameter(int si, int count, uint8_t param, float value)
{
    if( _audioUp && count > 0 && param < FAUN_PARAM_COUNT)
    {
        CommandA cmd;
        cmd.op     = CMD_PARAM_VOLUME + param;
        cmd.select = si;
        cmd.ext    = count;
        cmd.arg.f[0] = value;
        faun_command(&cmd, 8);
    }
}


/**
  Change volume of stereo channels over a period of time.

  \param si         Source or stream index.
  \param finalVolL  Target volume for left channel.
  \param finalVolR  Target volume for right channel.
  \param period     Number of seconds for transition.
*/
void faun_pan(int si, float finalVolL, float finalVolR, float period)
{
    if (_audioUp)
    {
        CommandA cmd;
        cmd.op     = CMD_VOLUME_VARY;
        cmd.select = si;
        cmd.ext    = 1;
        cmd.arg.f[0] = finalVolL;
        cmd.arg.f[1] = finalVolR;
        cmd.arg.f[2] = period;
        faun_command(&cmd, 16);
    }
}


/**
  Execute a Faun program.

  This can be used to sequence the playback of multiple sources and streams.

  Any currently running program on the execution unit will be halted and
  replaced.

  \param exec       Execution unit index.
  \param bytecode   FuanOpcode instructions and data.
                    The program must be terminated by FO_END.
  \param len        Byte length of bytecode.
                    The maximum len is #FAUN_PROGRAM_MAX.
*/
void faun_program(int exec, const uint8_t* bytecode, int len)
{
    if( _audioUp && exec < _pexecLimit )
    {
        uint8_t cmd[MSG_SIZE];
        const int payloadMax = MSG_SIZE - PROG_CHEAD;
        int clen;

        if (len > FAUN_PROGRAM_MAX || bytecode[len - 1] != FO_END)
            return;

        cmd[0] = (len > payloadMax) ? CMD_PROGRAM_BEG : CMD_PROGRAM;
        cmd[2] = exec;

        while (len > 0)
        {
            clen = (len > payloadMax) ? payloadMax : len;
            len -= clen;

            cmd[1] = clen;
            memcpy(cmd + PROG_CHEAD, bytecode, clen);
            bytecode += clen;
            tmsg_push(_voice.cmd, &cmd);

            cmd[0] = (len > payloadMax) ? CMD_PROGRAM_MID : CMD_PROGRAM_END;
        }
    }
}


static float _cmdSetBuffer(int bi, const FaunBuffer* buf)
{
    uint8_t cmd[MSG_SIZE];

    cmd[0] = CMD_SET_BUFFER;
    cmd[1] = bi;
    memcpy(cmd+2, buf, 16);
    tmsg_push(_voice.cmd, cmd);

    return (float) buf->used / (float) buf->rate;
}


/**
  Load a file into a PCM buffer.

  \param bi       Buffer index.
  \param file     Path to audio file.
  \param offset   Byte offset to the start of data in the file.
  \param size     Bytes to read from file. Pass zero to read to the file end.

  \return Duration in seconds or zero upon failure.

  \sa faun_loadBufferF(), faun_loadBufferPcm(), faun_loadBufferSfx()
*/
float faun_loadBuffer(int bi, const char* file, uint32_t offset, uint32_t size)
{
    float duration = 0.0f;

    if( _audioUp && bi < _bufferLimit )
    {
        FaunBuffer buf;
        const char* error;

        // Load buffer in user thread.
        FILE* fp = fopen(file, "rb");
        if (fp) {
            buf.sample.ptr = NULL;
            error = faun_readBuffer(&buf, fp, offset, size);
            if (error)
                fprintf(_errStream, "Faun %s (%s)\n", error, file);
            else
                duration = _cmdSetBuffer(bi, &buf);

            fclose(fp);
        } else {
            fprintf(_errStream, "Faun loadBuffer cannot open \"%s\"\n", file);
        }
    }
    return duration;
}


/**
  Load audio data from FILE into a PCM buffer.

  \param bi       Buffer index.
  \param fp       FILE pointer positioned at the start of audio data.
  \param size     Bytes to read from file. Pass zero to read to the file end.

  \return Duration in seconds or zero upon failure.

  \sa faun_loadBuffer()
*/
float faun_loadBufferF(int bi, FILE* fp, uint32_t size)
{
    if( _audioUp && bi < _bufferLimit )
    {
        FaunBuffer buf;
        const char* error;

        // Load buffer in user thread.
        buf.sample.ptr = NULL;
        error = faun_readBuffer(&buf, fp, 0, size);
        if (error)
            fprintf(_errStream, "Faun %s\n", error);
        else
            return _cmdSetBuffer(bi, &buf);
    }
    return 0.0f;
}


#ifdef USE_LOAD_MEM
/**
  Load PCM audio data from memory into a buffer.

  \param bi         Buffer index.
  \param format     FaunFormat mask of word size, channels, & sample rate.
  \param samples    Pointer to PCM samples.
  \param frames     Number of frames in samples.

  \return Duration in seconds or zero upon failure.

  \sa faun_loadBuffer()
*/
float faun_loadBufferPcm(int bi, int format, const void* samples,
                         uint32_t frames)
{
    if (_audioUp && bi < _bufferLimit) {
        FaunBuffer buf;
        uint32_t inFrames = frames;
        int chan = (format & FAUN_FMT_STEREO) ? 2 : 1;
        int rate;

        if (format & FAUN_FMT_22050) {
            frames *= 2;
            rate = 22050;
        } else {
            rate = 44100;
        }

        buf.sample.ptr = NULL;
        _allocBufferVoice(&buf, frames);
        buf.used = frames;

        if (format & FAUN_FMT_S16)
            convS16_F32(buf.sample.f32, (const int16_t*) samples, inFrames,
                        rate, chan);
        else
            convF32_F32(buf.sample.f32, (const float*) samples, inFrames,
                        rate, chan);

        return _cmdSetBuffer(bi, &buf);
    }
    return 0.0f;
}


#ifdef USE_SFX_GEN
/**
  Load audio data generated from SfxParams into a PCM buffer.

  \param bi         Buffer index.
  \param sfxParam   SfxParams/WaveParams struct.

  \return Duration in seconds or zero upon failure.

  \sa faun_loadBuffer()
*/
float faun_loadBufferSfx(int bi, const void* sfxParam)
{
    if (_audioUp && bi < _bufferLimit) {
        FaunBuffer buf;
        buf.sample.ptr = NULL;
        faun_generateSfx(&buf, (const SfxParams*) sfxParam);
        return _cmdSetBuffer(bi, &buf);
    }
    return 0.0f;
}
#endif
#endif


/**
  Free the memory used by a contiguous group of buffers.

  \param bi     First buffer index.
  \param count  Number of buffers to free.
*/
void faun_freeBuffers(int bi, int count)
{
    if( _audioUp ) {
        CommandA cmd;

        if (bi + count > _bufferLimit)
            count = _bufferLimit - bi;
        if (count < 1)
            return;

        cmd.op     = CMD_BUFFERS_FREE;
        cmd.select = bi;
        cmd.ext    = count;
        faun_command(&cmd, 4);
    }
}


static uint32_t faun_nextPlayId(int si)
{
    if (++_playSerialNo > 0xffffff)
        _playSerialNo = 1;
    uint32_t pid = (_playSerialNo << 8) | si;

    // The playback id is set in the caller's thread so that faun_isPlaying
    // can be used immediately after a faun_play* call.
    while (atomic_flag_test_and_set(&_pidLock)) {}
    _playbackId[si] = pid;
    atomic_flag_clear(&_pidLock);

    return pid;
}


/**
  Begin playback of a buffer from a source.

  The volume is set to either the current #FAUN_VOLUME parameter or 0.0 if
  mode includes #FAUN_PLAY_FADE_IN.
  To change the volume after playing has started use faun_pan().

  \param si     Source index.
  \param bi     Buffer indices.  Use the FAUN_PAIR() & FAUN_TRIO() macros to
                queue two or three buffers.
  \param mode   The FaunPlayMode (#FAUN_PLAY_ONCE or #FAUN_PLAY_LOOP).

  \returns Unique play identifier or zero if playback could not start.

  \sa faun_playSourceVol()
*/
uint32_t faun_playSource(int si, int bi, int mode)
{
    if( _audioUp )
    {
        uint32_t pid = faun_nextPlayId(si);
        CommandA cmd;
        cmd.op     = CMD_PLAY_SOURCE;
        cmd.select = si;
        cmd.ext    = mode;
        cmd.arg.u32[0] = bi;
        cmd.arg.u32[1] = pid;
        faun_command(&cmd, 12);
        return pid;
    }
    return NUL_PLAY_ID;
}


/**
  Begin playback of a buffer from a source and set channel volumes.

  This is similar to faun_playSource(), but the volume arguments override
  the #FAUN_VOLUME parameter setting.

  \param si     Source index.
  \param bi     Buffer indices.  Use the FAUN_PAIR() & FAUN_TRIO() macros to
                queue two or three buffers.
  \param mode   The FaunPlayMode (#FAUN_PLAY_ONCE or #FAUN_PLAY_LOOP).
  \param volL   Volume of left channel (0.0 to 1.0).
  \param volR   Volume of right channel (0.0 to 1.0).

  \returns Unique play identifier or zero if playback could not start.

  \sa faun_playSource()
*/
uint32_t faun_playSourceVol(int si, int bi, int mode, float volL, float volR)
{
    if( _audioUp )
    {
        uint32_t pid = faun_nextPlayId(si);
        CommandA cmd;
        cmd.op     = CMD_PLAY_SOURCE_VOL;
        cmd.select = si;
        cmd.ext    = mode;
        cmd.arg.u32[0] = bi;
        cmd.arg.u32[1] = pid;
        cmd.arg.f[2] = volL;
        cmd.arg.f[3] = volR;
        faun_command(&cmd, 20);
        return pid;
    }
    return NUL_PLAY_ID;
}


/**
  Open a file and optionally begin streaming.

  Reading can be limited to a specific chunk of the file if the size argument
  is non-zero.

  To begin playback the mode must include either #FAUN_PLAY_ONCE or
  #FAUN_PLAY_LOOP.  If it does not, then a #FC_START command or a call to
  faun_playStreamPart() must be used to initiate play.

  \param si     Stream index.
  \param file   File path.
  \param offset Byte offset to start of stream data in file.
  \param size   Byte size of stream data in file, or zero if the entire file
                is to be used.
  \param mode   The FaunPlayMode (#FAUN_PLAY_ONCE, #FAUN_PLAY_LOOP, etc.).

  \returns Unique play identifier or zero if streaming could not start.
*/
uint32_t faun_playStream(int si, const char* file, uint32_t offset,
                         uint32_t size, int mode)
{
    if( _audioUp )
    {
        FILE* fp = fopen(file, "rb");
        if (fp)
        {
            uint32_t pid = faun_nextPlayId(si);
            CommandA cmd;
            cmd.op     = CMD_OPEN_STREAM;
            cmd.select = si;
            cmd.ext    = mode;
            cmd.arg.u32[0] = pid;
            cmd.arg.u32[1] = offset;
            memcpy(&cmd.arg.u32[2], &fp, sizeof(void*));
#if __SIZEOF_POINTER__ == 4
            cmd.arg.u32[3] = size;
            faun_command(&cmd, 20);
#else
            if (size) {
                // Pass the size argument first in it's own message.
                cmd.op = CMD_OPEN_STREAM_SIZE;
                cmd.arg.u32[1] = size;
                faun_command(&cmd, 12);

                cmd.op = CMD_OPEN_STREAM;
                cmd.arg.u32[1] = offset;
            }
            faun_command(&cmd, 20);
#endif
            return pid;
        }
        else
            fprintf(_errStream, "Faun playStream cannot open \"%s\"\n", file);
    }
    return NUL_PLAY_ID;
}


/**
  Begin playing a segment from a stream.

  The stream must have been previously initialized by faun_playStream().

  \param si         Stream index.
  \param start      Playback start position (in seconds).
  \param duration   Time to play (in seconds).
  \param mode       FaunPlayMode (#FAUN_PLAY_ONCE or #FAUN_PLAY_LOOP).
*/
void faun_playStreamPart(int si, double start, double duration, int mode)
{
    if( _audioUp )
    {
        const int sd = sizeof(double);
        uint8_t cmdBuf[MSG_SIZE];
        CommandA* cmd = (CommandA*) cmdBuf;

        cmd->op = CMD_PLAY_STREAM_PART;
        cmd->select = si;
        cmd->ext = mode;
        memcpy(cmdBuf+4, &start, sd);
        memcpy(cmdBuf+4+sd, &duration, sd);

        faun_command(cmdBuf, 20);
    }
}


/**
  Check if a source or stream is still playing.

  \param pid    Playback identifier returned by faun_playSource(),
                faun_playSourceVol(), or faun_playStream().

  \returns Non-zero if source is playing.
*/
int faun_isPlaying(uint32_t pid)
{
    if (_audioUp && pid != NUL_PLAY_ID)
    {
        int si = FAUN_PID_SOURCE(pid);
        assert(si < _sourceLimit + _streamLimit);
        if (_playbackId[si] == pid)
           return 1;
    }
    return 0;
}
