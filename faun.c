/*  ___________
    \_   _____/____   __ __  ____
     |    __) \__  \ |  |  \/    \
     |    \    / __ \|  |  /   |  \
     \__  /   (____  /____/|___|  /
        \/         \/           \/

  Faun - A high-level C audio library

  Copyright (c) 2022 Karl Robillard

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

#ifdef __linux__
#include "pulseaudio.c"
#elif defined(_WIN32)
//#include "dsound.c"
#include "wasapi.c"
#else
#error "Unsupported system"
#endif

#include <vorbis/codec.h>
#include <vorbis/vorbisfile.h>
#ifdef __ANDROID__
#include <glv_asset.h>
#endif

#ifdef CAPTURE
#include "wav_write.c"
FILE* wfp = NULL;
int endOnSignal = 0;
int endCapture = 0;
#endif

#if 0
#define REPORT_STREAM(st,msg) \
    printf("FAUN strm %x: %s\n",st->source.serialNo,msg)
#else
#define REPORT_STREAM(st,msg)
#endif

typedef struct {
#ifdef __ANDROID__
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
        uint16_t u16[6];
        uint32_t u32[3];
        float f[3];
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
    CMD_SET_BUFFER,
    CMD_BUFFERS_FREE,
    CMD_PLAY_SOURCE,
    CMD_OPEN_STREAM,
    CMD_PLAY_STREAM_PART,

    CMD_CON_START,
    CMD_CON_STOP,
    CMD_CON_RESUME,
    CMD_CON_FADE_OUT,

    CMD_PARAM_VOLUME,
    CMD_PARAM_PAN,
    CMD_PARAM_FADE_PERIOD,
    CMD_PARAM_END_TIME,

    CMD_COUNT
};

#define MSG_SIZE    20
#define SIG_SIZE    2
#define PROG_MAX    32

typedef struct {
    uint8_t code[PROG_MAX];
    int pc;
    int used;
    int si;
    uint32_t waitPos;
}
FaunProgram;

enum SourceState {
    SS_UNUSED,
    SS_PLAYING,
    SS_STOPPED
};

#define QACTIVE_NONE    0xffff
#define END_POS_NONE    0x7fffffff
#define SOURCE_QUEUE_SIZE   4
#define BID_PACKED      0x3ff
#define SOURCE_ID(src)  (src->serialNo & 0xff)

typedef struct {
    uint16_t state;         // SourceState
    uint16_t bufUsed;       // Number of buffers in queue.
    uint16_t qtail;         // Queue index of append position.
    uint16_t qhead;
    uint16_t qactive;       // Queue index of currently playing buffer.
    uint16_t mode;
    float    gain;          // Current gain.
    float    fade;          // Gain delta per update cycle.
    //uint32_t signalTime;
    float    volume;        // FAUN_VOLUME, user specified gain.
    float    pan;           // FAUN_PAN
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


static void faun_sourceInit(FaunSource* src, int serial)
{
    memset(src, 0, sizeof(FaunSource));
    src->qactive = QACTIVE_NONE;
    src->gain = 1.0f;
    //src->fade = 0.0f;
    src->volume = 1.0f;
    //src->pan  = 0.0f;
    src->fadePeriod = 1.5f;
    src->serialNo = serial;
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
FaunBuffer* faun_processedBuffer(FaunSource* src)
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
    FaunSource  source;
    FaunBuffer  buffers[STREAM_BUFFERS];
    int         bufAvail;
    int16_t     feed;
    int16_t     _pad;
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

#define UPDATE_HZ   45
#define SLEEP_MS    (1000/UPDATE_HZ - 2)
#define BUFFER_MAX  256
#define SOURCE_MAX  32
#define STREAM_MAX  6

static int _audioUp = AUDIO_DOWN;
static int _bufferLimit;
static int _sourceLimit;
static int _streamLimit;
static FaunVoice   _voice;
static FaunBuffer* _abuffer = NULL;
static FaunSource* _asource = NULL;
static StreamOV*  _stream = NULL;

//----------------------------------------------------------------------------

#include "wav_read.c"

#ifdef USE_FLAC
#include "flac.c"
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

static void convertMono(float*, float*, float**, const float*);
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

// Return number of frames or zero upon error.
static uint32_t cmd_bufferFile(FaunBuffer* buf, const char* path,
                               uint32_t offset, uint32_t size)
{
    WavHeader wh;
    FILE* fp;
    uint32_t frames = 0;
    int err;

    fp = fopen(path, "rb");
    if (! fp) {
        fprintf(_errStream, "Faun loadBuffer cannot open \"%s\"\n", path);
        return 0;
    }
    if (offset)
        fseek(fp, offset, SEEK_SET);

    err = wav_readHeader(fp, &wh);
    if (err == 0) {
        int16_t* readBuf;
        size_t n;
        uint32_t wavFrames;

        //wav_dumpHeader(stdout, &wh, NULL, "  ");

        if (wh.sampleRate != 44100 && wh.sampleRate != 22050) {
            fprintf(_errStream, "WAVE sample rate %d is unsupported\n",
                    wh.sampleRate);
            goto cleanup;
        }
        if (wh.bitsPerSample != 16) {
            fprintf(_errStream, "WAVE bits per sample is not 16\n");
            goto cleanup;
        }

        frames = wavFrames = wav_sampleCount(&wh);
        if (wh.sampleRate == 22050)
            frames *= 2;
        _allocBufferVoice(buf, frames);

        readBuf = (int16_t*) malloc(wh.dataSize);
        n = fread(readBuf, 1, wh.dataSize, fp);
        if (n != wh.dataSize) {
            fprintf(_errStream, "Faun cannot read WAVE \"%s\"\n", path);
        } else {
            buf->used = frames;
            convS16_F32(buf->sample.f32, readBuf, wavFrames, wh.sampleRate,
                        wh.channels);
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
        faun_sourceInit(&os.source, -1);
        os.sampleCount = 0;

        os.chunk.cfile  = fp;
        os.chunk.offset = offset;
        os.chunk.size   = size;

        // wav_readHeader() has read 20 bytes for us.
        if (ov_open_callbacks(&os.chunk, &os.vf, (char*) &wh, 20,
                              chunkMethods) < 0)
        {
            fprintf(_errStream, "Faun cannot open Ogg \"%s\"\n", path);
        }
        else
        {
            os.vinfo = ov_info(&os.vf, -1);
            frames = ov_pcm_total(&os.vf, -1);
            //printf("FAUN ogg frame:%d chan:%d\n", frames, os.vinfo->channels);
            _allocBufferVoice(buf, frames);
            status = _readOgg(&os, buf);
            if (status != RSTAT_DATA)
                fprintf(_errStream, "Faun cannot read Ogg \"%s\"\n", path);

            ov_clear(&os.vf);  // Closes fp for us.
            return frames;
        }
      }
#ifdef USE_FLAC
      else if (wh.idRIFF == ID_FLAC)
      {
        const int bufSize = 128;
        const int decodeSize = 512;
        uint8_t* readBuf;
        int32_t* decodeBuf;
        size_t toRead, n;
        uint32_t inUsed, procLen;
        uint32_t bufPos = 0;
        //uint32_t frate;
        uint32_t fchannels;
        float* pcmOut = NULL;
        fx_flac_state_t fstate;
        fx_flac_t* flac = FX_FLAC_ALLOC_SUBSET_FORMAT_DAT();

        decodeBuf = (int32_t*) malloc(decodeSize*sizeof(int32_t) + bufSize);
        readBuf   = (uint8_t*) (decodeBuf + decodeSize);

        // wav_readHeader() has read 20 bytes for us.
        memcpy(readBuf, &wh, 20);
        bufPos = 20;

        while (1) {
            toRead = bufSize - bufPos;
            if (toRead > 0) {
                n = fread(readBuf + bufPos, 1, toRead, fp);
                if (n == 0)
                    break;
                bufPos += n;    // Advance the write cursor
            }

            inUsed  = bufPos;
            procLen = decodeSize;
            fstate = fx_flac_process(flac, readBuf, &inUsed,
                                           decodeBuf, &procLen);
            if (fstate == FLAC_ERR) {
                fprintf(_errStream, "Faun cannot decode FLAC \"%s\"\n", path);
                break;
            }
            if (fstate == FLAC_END_OF_METADATA) {
              //frate     = fx_flac_get_streaminfo(flac, FLAC_KEY_SAMPLE_RATE);
                fchannels = fx_flac_get_streaminfo(flac, FLAC_KEY_N_CHANNELS);
                frames    = fx_flac_get_streaminfo(flac, FLAC_KEY_N_SAMPLES);

                //printf("FLAC rate:%d channels:%d samples:%d\n",
                //        frate, fchannels, frames);

                // NOTE: Zero for total samples denotes 'unknown' and is valid.
                if (! frames) {
                    fprintf(_errStream, "FLAC total samples is unknown!\n");
                    break;
                }

                _allocBufferVoice(buf, frames);
                pcmOut = buf->sample.f32;
            }

            // Save decoded samples to PCM buffer.
            if (pcmOut) {
                if (fchannels == 1) {
                    for (uint32_t i = 0; i < procLen; i++) {
                        float ds = (decodeBuf[i] >> 16) / 32767.0f;
                        *pcmOut++ = ds;
                        *pcmOut++ = ds;
                    }
                } else {
                    for (uint32_t i = 0; i < procLen; i++)
                        *pcmOut++ = (decodeBuf[i] >> 16) / 32767.0f;
                }
            }

            // Move unprocessed bytes to the beginning of readBuf.
            n = bufPos - inUsed;
            memmove(readBuf, readBuf + inUsed, n);
            bufPos = n;
        }
        buf->used = frames;

        free(decodeBuf);
        free(flac);
      }
#endif
#ifdef USE_SFX_GEN
      else if (wh.idRIFF == ID_RFX_)
      {
        SfxParams sfx;
        SfxSynth* synth;
        float* dst;
        float* src;
        float gain[2];
        int version = ((uint16_t*) &wh)[2];
        if (version != 200)
            fprintf(_errStream, "rFX file version not supported\n");
        else {
            fseek(fp, offset + 8, SEEK_SET);
            if (fread(&sfx, 1, sizeof(SfxParams), fp) == sizeof(SfxParams)) {
                synth = sfx_allocSynth(SFX_F32, 44100, 6);
                faun_randomSeed(&_rng, sfx.randSeed);
                frames = sfx_generateWave(synth, &sfx);

                _allocBufferVoice(buf, frames);
                gain[0] = gain[1] = 1.0f;
                dst = buf->sample.f32;
                src = synth->samples.f;
                convertMono(dst, dst + frames*2, &src, gain);
                buf->used = frames;

                free(synth);
            }
            else
                fprintf(_errStream, "Faun cannot read rFX \"%s\"\n", path);
        }
      }
#endif
    }

cleanup:
    fclose(fp);
    return frames;
}

//----------------------------------------------------------------------------

static void stream_init(StreamOV* st, int id)
{
    faun_sourceInit(&st->source, id);
    memset(&st->buffers, 0, sizeof(FaunBuffer) * STREAM_BUFFERS);
    st->feed = 0;

#ifdef __ANDROID__
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


static void faun_panGain(float pan, float* gain)
{
    gain[0] = (pan <= 0.0f) ? 1.0f : 1.0f - pan;
    gain[1] = (pan >= 0.0f) ? 1.0f : 1.0f + pan;
}


static void convertStereoHR(float* dst, float* end, float** src,
                            const float* gain)
{
    const float* ls = src[0];
    const float* rs = src[1];
    float L, R;
    while (dst != end) {
        L = *ls++ * gain[0];
        R = *rs++ * gain[1];
        *dst++ = L;
        *dst++ = R;
        *dst++ = L;
        *dst++ = R;
    }
}

// Interleave the separate channel data and apply panning.
static void convertStereo(float* dst, float* end, float** src,
                          const float* gain)
{
    const float* ls = src[0];
    const float* rs = src[1];
    while (dst != end) {
        *dst++ = *ls++ * gain[0];
        *dst++ = *rs++ * gain[1];
    }
}

static void convertMonoHR(float* dst, float* end, float** src,
                          const float* gain)
{
    const float* c0 = src[0];
    float L, R;
    while (dst != end) {
        L = *c0   * gain[0];
        R = *c0++ * gain[1];
        *dst++ = L;
        *dst++ = R;
        *dst++ = L;
        *dst++ = R;
    }
}

static void convertMono(float* dst, float* end, float** src, const float* gain)
{
    const float* c0 = src[0];
    float C;
    while (dst != end) {
        C = *c0++;
        *dst++ = C * gain[0];
        *dst++ = C * gain[1];
    }
}

typedef void (*StreamConvertFunc)(float*, float*, float**, const float*);


/*
  Decode some audio, copy it into a buffer, and update sampleCount.
  Returns a mask of ReadOggStatus bits.
*/
static int _readOgg(StreamOV* st, FaunBuffer* buffer)
{
    float** oggPcm;
    float* dst;
    StreamConvertFunc convert;
    float gain[2];
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

    faun_panGain(st->source.pan, gain);

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
        convert(dst, dst + amt*2, oggPcm, gain);
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
        //printf( "FAUN ogg count %d\n", count );
        st->sampleCount += count;
        //wav_write(wfp, buffer->sample.f32, count*2);
    }
    return status;
}


static void stream_start(StreamOV* st)
{
    FaunBuffer* buf;
    int frameCount, i;
    int rate = _voice.mix.rate;

    // Size each buffer to hold 1/4 second of data (multiple of 8 samples).
    frameCount = ((rate / 4) + 7) & ~7;

    faun_sourceResetQueue(&st->source);

    for( i = 0; i < STREAM_BUFFERS; ++i )
    {
        buf = st->buffers+i;

        // Match attributes of voice mixing buffer.
        if (! buf->sample.ptr)
            faun_allocBufferSamples(buf, FAUN_F32, FAUN_CHAN_2, rate,
                                    frameCount);

        if( _readOgg(st, buf) & (RSTAT_ERROR | RSTAT_EOF) )
        {
            stream_closeFile(st);
            break;
        }
    }

    if( i == STREAM_BUFFERS )
    {
        for( i = 0; i < STREAM_BUFFERS; ++i )
            faun_queueBuffer(&st->source, st->buffers+i);
        st->source.state = SS_PLAYING;
        st->source.playPos =
        st->source.framesOut = 0;
        st->feed = 1;
        REPORT_STREAM(st,"start");
    }
}


static void stream_stop(StreamOV* st)
{
    REPORT_STREAM(st,"stop");
    st->source.state = SS_STOPPED;
    st->feed = 0;

    if( st->chunk.cfile )
        stream_closeFile( st );
}


static void signalDone(const FaunSource* src)
{
    FaunSignal sig;

    sig.id = SOURCE_ID(src);
    sig.signal = FAUN_SIGNAL_DONE;
    //printf("signalDone %x\n", sig.id);

    tmsg_push(_voice.sig, &sig);

#ifdef CAPTURE
    if (endOnSignal)
        endCapture = 1;
#endif
}


#define FADE_DELTA(period)  ((1.0f / UPDATE_HZ) / period * src->volume)
#define GAIN_SILENCE_THRESHOLD  0.001f

static inline void source_fadeOut(FaunSource* src)
{
    src->fade = -FADE_DELTA(src->fadePeriod);
}


// Only called if src->fade != 0.0f.
static void source_fade(FaunSource* src, StreamOV* st)
{
    src->gain += src->fade;
    //printf("KR fade 0x%x %f %f\n", src->mode, src->fade, src->gain);
    if (src->fade > 0.0f) {
        if (src->gain >= src->volume) {
            src->gain = src->volume;
            src->fade = 0.0f;
        }
    } else if (src->gain <= GAIN_SILENCE_THRESHOLD) {
        src->gain = 0.0f;
        src->fade = 0.0f;
        if (src->mode & FAUN_SIGNAL_DONE)
            signalDone(src);
        if (st)
            stream_stop(st);
    }
}


static void source_setMode(FaunSource* src, int mode)
{
    src->mode = mode;

    if (mode & FAUN_PLAY_FADE_IN) {
        src->gain = 0.0f;
        src->fade = FADE_DELTA(src->fadePeriod);
    }
    src->fadePos = END_POS_NONE;
}


static void cmd_playSource(int si, uint32_t bufIds, int mode)
{
    FaunSource* src = _asource + si;
    FaunBuffer* buf = _abuffer + (bufIds & BID_PACKED);
    uint32_t ftotal;

    //printf("CMD source play %d buf:%x mode:%d\n", si, bufIds, mode);
    faun_setBuffer(src, buf);
    ftotal = buf->used;
    for (bufIds >>= 10; bufIds; bufIds >>= 10) {
         buf = _abuffer + ((bufIds-1) & BID_PACKED);
        faun_queueBuffer(src, buf);
        ftotal += buf->used;
    }

    src->playPos = src->framesOut = 0;
    source_setMode(src, mode);

    if (mode & FAUN_PLAY_FADE_OUT) {
        uint32_t ff = (uint32_t) (src->fadePeriod * 44100.0f);
        // Avoiding overlap with any fade-in.
        if (ftotal > 2*ff)
            src->fadePos = ftotal - ff;
    }

    if (mode & (FAUN_PLAY_ONCE | FAUN_PLAY_LOOP))
        src->state = SS_PLAYING;
    else
        src->state = SS_STOPPED;
}


static void cmd_playStream(int si, const FileChunk* fc, int mode)
{
    StreamOV* st = _stream + (si - _sourceLimit);
    stream_stop( st );

    st->chunk = *fc;
    if (fc->offset)
        fseek(fc->cfile, fc->offset, SEEK_SET);

    if (ov_open_callbacks(&st->chunk, &st->vf, NULL, 0, chunkMethods) < 0)
    {
#ifdef __ANDROID__
        glv_assetClose(&st->asset);
#else
        fclose(st->chunk.cfile);
        st->chunk.cfile = NULL;
#endif
        fprintf(_errStream, "Faun cannot open Ogg (stream %d)\n", si);
    }
    else
    {
        st->feed = 0;
        st->sampleCount = 0;
        st->sampleLimit = 0;
        st->vinfo = ov_info(&st->vf, -1);

        source_setMode(&st->source, mode);

        if (mode & FAUN_PLAY_FADE_OUT) {
            uint32_t frames = ov_pcm_total(&st->vf, -1);
            uint32_t ff = (uint32_t) (st->source.fadePeriod * 44100.0f);
            // Avoiding overlap with any fade-in.
            if (frames > 2*ff)
                st->source.fadePos = frames - ff;
        }

        if (mode & (FAUN_PLAY_ONCE | FAUN_PLAY_LOOP))
            stream_start(st);
    }
}


static void cmd_playStreamPart(int si, double start, double duration, int mode)
{
    StreamOV* st = _stream + (si - _sourceLimit);

    st->feed = 0;
    st->start = start;
    st->sampleCount = 0;
    st->sampleLimit = (uint32_t) (duration * _voice.mix.rate);
    //printf("KR rate %d %d\n", st->vinfo->rate, st->sampleLimit);

    source_setMode(&st->source, mode);

    st->source.state = SS_STOPPED;

    ov_time_seek(&st->vf, st->start);
    stream_start(st);
}


/**
  Called periodically (once per frame) to decode audio from file.
  Should only be called if st->feed and st->chunk.cfile are both non-zero.
*/
static void stream_fillBuffers(StreamOV* st)
{
    FaunBuffer* freeBuf;
    int status;

    while ((freeBuf = faun_processedBuffer(&st->source)))
    {
        //printf( "   buf %ld\n", freeBuf - st->buffers);
read_again:
        status = _readOgg(st, freeBuf);
        if( status & RSTAT_DATA )
        {
            faun_queueBuffer(&st->source, freeBuf);
            if (SEGMENT_SET(st) && st->sampleCount >= st->sampleLimit)
            {
                freeBuf->used -= st->sampleCount - st->sampleLimit;
                status |= RSTAT_EOF;
            }
        }

        if( status & RSTAT_ERROR )
        {
            stream_closeFile( st );
            break;
        }
        else if( status & RSTAT_EOF )
        {
            //printf( "KR audioUpdate - end of stream\n" );
            if (st->source.mode & FAUN_PLAY_LOOP)
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
}

//----------------------------------------------------------------------------

static void _mix1Buffer(float* output, float* end, const float* input,
                        float gain, int pass)
{
    if (pass == 0) {
        while (output != end)
            *output++ = *input++ * gain;
    } else {
        while (output != end) {
            *output += *input++ * gain;
            output++;
        }
    }
}

static void _mix2Buffers(float* output, float* end, const float** input,
                         const float* gain, int pass)
{
    const float* in0 = input[0];
    const float* in1 = input[1];
    if (pass == 0) {
        while (output != end)
            *output++ = (*in0++ * gain[0]) + (*in1++ * gain[1]);
    } else {
        while (output != end) {
            *output += (*in0++ * gain[0]) + (*in1++ * gain[1]);
            output++;
        }
    }
}

static void _mix4Buffers(float* output, float* end, const float** input,
                         const float* gain, int pass)
{
    const float* in0 = input[0];
    const float* in1 = input[1];
    const float* in2 = input[2];
    const float* in3 = input[3];
    if (pass == 0) {
        while (output != end)
            *output++ = (*in0++ * gain[0]) + (*in1++ * gain[1]) +
                        (*in2++ * gain[2]) + (*in3++ * gain[3]);
    } else {
        while (output != end) {
            *output += (*in0++ * gain[0]) + (*in1++ * gain[1]) +
                       (*in2++ * gain[2]) + (*in3++ * gain[3]);
            output++;
        }
    }
}


/*
 * Inputs with gain below GAIN_SILENCE_THRESHOLD will be skipped.
 */
void faun_mixBuffers(float* output, const float** input, const float* inGain,
                    int inCount, uint32_t sampleCount)
{
    float* end = output + sampleCount;
    const float* inputQ[4];
    float gainQ[4];
    int i;
    int pass = 0;
    int q = 0;

    for (i = 0; i < inCount; i++) {
        if (inGain[i] > GAIN_SILENCE_THRESHOLD) {
            inputQ[q] = input[i];
            gainQ[q] = inGain[i];
            if (q == 3) {
                q = 0;
                _mix4Buffers(output, end, inputQ, gainQ, pass++);
            } else
                q++;
        }
    }

    switch (q) {
        case 3:
            _mix2Buffers(output, end, inputQ, gainQ, pass++);
            _mix1Buffer(output, end, inputQ[2], gainQ[2], pass);
            break;
        case 2:
            _mix2Buffers(output, end, inputQ, gainQ, pass);
            break;
        case 1:
            _mix1Buffer(output, end, inputQ[0], gainQ[0], pass);
            break;
        default:
            if (pass == 0)
                memset(output, 0, sampleCount * sizeof(float));
            break;
    }
}

//----------------------------------------------------------------------------

static void faun_evalProg(FaunProgram* prog, uint32_t mixClock)
{
    const uint8_t* pc;
    const uint8_t* end;
    FaunSource* src;
    int i;

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

            case FO_QUEUE_DONE:
                i = FAUN_PLAY_ONCE | FAUN_SIGNAL_DONE;
playSrc:
                if (prog->si < _sourceLimit)
                    cmd_playSource(prog->si, *pc++, i);
                break;

            case FO_QUEUE_FADE:
                i = FAUN_PLAY_ONCE | FAUN_PLAY_FADE;
                goto playSrc;

            case FO_QUEUE_FADE_DONE:
                i = FAUN_PLAY_ONCE | FAUN_PLAY_FADE | FAUN_SIGNAL_DONE;
                goto playSrc;
                break;

            case FO_PLAY_ONCE:
                if (prog->si < _sourceLimit)
                    cmd_playSource(prog->si, *pc++, FAUN_PLAY_ONCE);
                break;

            case FO_PLAY_LOOP:
                if (prog->si >= _sourceLimit) {
                    StreamOV* st = _stream + (prog->si - _sourceLimit);
                    st->source.mode |= FAUN_PLAY_LOOP;
                    stream_start(st);
                }
                break;

            case FO_SET_VOL:
                i = prog->si;
                src = (i < _sourceLimit) ? _asource + i
                                         : &_stream[i - _sourceLimit].source;
                src->volume = src->gain = (float) (*pc++) / 255.0f;
                break;

            case FO_SET_PAN:
                i = prog->si;
                src = (i < _sourceLimit) ? _asource + i
                                         : &_stream[i - _sourceLimit].source;
                src->pan = (float) (*pc++) / 255.0f;
                break;

            case FO_SET_FADE:
                i = prog->si;
                src = (i < _sourceLimit) ? _asource + i
                                         : &_stream[i - _sourceLimit].source;
                src->fadePeriod = (float) (*pc++) / 10.0f;
                break;

            case FO_SET_END:
                i = prog->si;
                src = (i < _sourceLimit) ? _asource + i
                                         : &_stream[i - _sourceLimit].source;
            {
                uint32_t pos = *pc++;
                _asource[prog->si].endPos = pos ? pos * 4410 : END_POS_NONE;
            }
                break;

            case FO_LOOP_ON:
            case FO_LOOP_OFF:
                i = prog->si;
                src = (i < _sourceLimit) ? _asource + i
                                         : &_stream[i - _sourceLimit].source;
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
                i = prog->si;
                src = (i < _sourceLimit) ? _asource + i
                                         : &_stream[i - _sourceLimit].source;
            {
                uint16_t mode = src->mode & ~FAUN_PLAY_FADE;
                if (pc[-1] == FO_FADE_ON)
                    mode |= FAUN_PLAY_FADE;
                src->mode = mode;
            }
                break;
            */

            case FO_FADE_IN:
                i = prog->si;
                src = (i < _sourceLimit) ? _asource + i
                                         : &_stream[i - _sourceLimit].source;
                src->gain = 0.0f;
                src->fade = FADE_DELTA(src->fadePeriod);
                break;

            case FO_FADE_OUT:
                i = prog->si;
                src = (i < _sourceLimit) ? _asource + i
                                         : &_stream[i - _sourceLimit].source;
                source_fadeOut(src);
                break;

            case FO_SIGNAL:
            {
                FaunSignal sig;
                sig.id = prog->si;
                sig.signal = FAUN_SIGNAL_DONE;
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
    FaunProgram prog;
    FaunVoice* voice = arg;
    FaunSource* src;
    FaunBuffer* buf;
    StreamOV* st;
    FaunSource** mixSource;
    const float** input;
    float* inputGain;
    const char* error;
    char cmdBuf[sizeof(CommandF) * 2];
    CommandA* cmd = (CommandA*) cmdBuf;
    int sourceCount;
    uint32_t mixed;
    uint32_t totalMixed = 0;    // Wraps after 27 hours.
    uint32_t fragmentLen;
    uint32_t samplesAvail;
    uint32_t mixSampleLen = voice->mix.avail;
    int i;
    struct MsgPort* port = voice->cmd;
    MsgTime ts;
    int sleepTime = SLEEP_MS;
    int n;

#ifdef CPUCOUNTER_H
    uint64_t t, t1, t2;
#define COUNTER(V)  V = cpuCounter()
#else
#define COUNTER(V)
#endif

#ifdef _WIN32
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
#endif

    n = _sourceLimit + _streamLimit;
    mixSource = (FaunSource**) malloc(n * sizeof(void*) * 3);
    input     = (const float**) (mixSource + n);
    inputGain = (float*) (input + n);

    prog.pc = prog.used = prog.si = 0;
    prog.waitPos = 0;

    tmsg_setTimespec(&ts, sleepTime);

    for (;;)
    {
        // Wait for commands.
        COUNTER(t);
        if (sleepTime > 0)
        {
            n = tmsg_popTimespec(port, cmd, &ts);
        }
        else
            n = tmsg_pop(port, cmd);
#ifdef CPUCOUNTER_H
        printf("KR msg   %9ld\n", cpuCounter() - t);
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
                    sleepTime = SLEEP_MS;
                    sysaudio_startVoice(voice);
                    break;

                case CMD_PROGRAM:
                    n = cmd->select;
                    if (prog.used + n > PROG_MAX)
                        fprintf(_errStream, "Program buffer overflow\n");
                    else {
                        memcpy(prog.code + prog.used, cmdBuf + 2, n);
                        prog.used += n;
                    }
                    break;

                case CMD_SET_BUFFER:
                    //printf("CMD set buffer bi:%d cmdPart:%d\n",
                    //       cmd->select, cmdPart);
                    buf = _abuffer + cmd->select;
                    free(buf->sample.ptr);
                    // TODO: Detach from all sources.

                    // Command contains sample, avail, & used members.
                    memcpy(buf, cmdBuf + 2, 16);

                    // The other parameters match _allocBufferVoice().
                    buf->rate = voice->mix.rate;
                    buf->format = FAUN_F32;
                    buf->chanLayout = FAUN_CHAN_2;
                    break;

                case CMD_BUFFERS_FREE:
                    faun_freeBufferSamples(cmd->ext, _abuffer + cmd->select);
                    // TODO: Detach from all sources.
                    break;

                case CMD_PLAY_SOURCE:
                    cmd_playSource(cmd->select, cmd->arg.u32[0], cmd->ext);
                    break;

                case CMD_OPEN_STREAM:
                {
                    FileChunk fc;
                    memcpy(&fc, cmdBuf+4, sizeof(fc));
                    //printf("CMD open stream %d %d %d\n",
                    //       cmd->select, fc.offset, cmd->ext);
                    cmd_playStream(cmd->select, &fc, cmd->ext);
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

                case CMD_CON_START:
                case CMD_CON_STOP:
                case CMD_CON_RESUME:
                    i = cmd->select;
                    n = i + cmd->ext;
                    for ( ; i < n; ++i) {
                        if (i < _sourceLimit)
                            src = _asource + i;
                        else
                            src = &_stream[i - _sourceLimit].source;
                        src->state = (cmd->op == CMD_CON_STOP) ? SS_STOPPED
                                                               : SS_PLAYING;
                    }
                    break;

                case CMD_CON_FADE_OUT:
                    //printf("CMD control %x %d\n", cmd->select,
                    //        cmd->op - CMD_CON_START);
                    i = cmd->select;
                    n = i + cmd->ext;
                    for ( ; i < n; ++i) {
                        if (i < _sourceLimit)
                            src = _asource + i;
                        else
                            src = &_stream[i - _sourceLimit].source;
                        source_fadeOut(src);
                    }
                    break;

                case CMD_PARAM_VOLUME:
                case CMD_PARAM_PAN:
                case CMD_PARAM_FADE_PERIOD:
                {
                    float* ppar;
                    int pi = cmd->op - CMD_PARAM_VOLUME;

                    //printf("CMD param %d:%d %d %f\n",
                    //       cmd->select, cmd->ext, pi, cmd->arg.f[0]);
                    i = cmd->select;
                    n = i + cmd->ext;
                    for ( ; i < n; ++i) {
                        if (i < _sourceLimit)
                            src = _asource + i;
                        else
                            src = &_stream[i - _sourceLimit].source;

                        ppar = &src->volume;
                        ppar[pi] = cmd->arg.f[0];
                        if (pi == FAUN_VOLUME)
                            src->gain = ppar[pi];
                    }
                }
                    break;

                case CMD_PARAM_END_TIME:
                    n = cmd->select;
                    if (n < _sourceLimit)
                        src = _asource + n;
                    else
                        src = &_stream[n - _sourceLimit].source;

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

        if (prog.used)
            faun_evalProg(&prog, totalMixed);

        COUNTER(t);

        // Collect active sources.
        sourceCount = 0;
        for (i = 0; i < _sourceLimit; ++i)
        {
            src = _asource + i;
            if (src->state == SS_PLAYING)
            {
                if (src->fade)
                    source_fade(src, NULL);
                mixSource[sourceCount++] = src;
            }
        }

        // Read streams and collect their sources.
        for (i = 0; i < _streamLimit; ++i)
        {
            st = _stream + i;
            if (st->source.state == SS_PLAYING)
            {
                if (st->source.fade)
                    source_fade(&st->source, st);
                if (st->feed && st->chunk.cfile)
                    stream_fillBuffers(st);
                if (st->source.qactive != QACTIVE_NONE)
                    mixSource[sourceCount++] = &st->source;
            }
        }

        // Mix active sources into voice buffer.
        for (mixed = 0; mixed < mixSampleLen; )
        {
            // Determine size of fragment for this mix pass.
            fragmentLen = mixSampleLen - mixed;
            for (i = 0; i < sourceCount; ++i)
            {
                src = mixSource[i];
                if (src->qactive == QACTIVE_NONE)
                    inputGain[i] = 0.0f;
                else
                {
                    buf = src->bufferQueue[src->qactive];
                    input[i] = buf->sample.f32 + src->playPos*2;
                    inputGain[i] = src->gain;

                    samplesAvail = buf->used - src->playPos;
                    if (samplesAvail < fragmentLen)
                        fragmentLen = samplesAvail;
                }

                //printf("KR     source %d qactive:%d pos:%d\n",
                //       i, src->qactive, src->playPos);
            }

            // Mix fragment.
            //printf("FAUN mix count:%d mixed:%4d/%d frag:%4d\n",
            //       sourceCount, mixed, mixSampleLen, fragmentLen);
            faun_mixBuffers(voice->mix.sample.f32 + mixed*2,
                           input, inputGain, sourceCount, fragmentLen*2);

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
                        src->qactive = QACTIVE_NONE;
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
                            } else
                                src->qactive = n;
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

        COUNTER(t1);
        error = sysaudio_write(voice, voice->mix.sample.f32,
                               mixed*2 * sizeof(float));
#ifdef CPUCOUNTER_H
        COUNTER(t2);
        printf("KR mix   %9ld\n"
               "   write %9ld\n", t1 - t, t2 - t1);
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

  \def FAUN_PAIR(a,b)
  Used with faun_playSource() to queue two buffers that will be played
  sequentially.

  \def FAUN_TRIO(a,b,c)
  Used with faun_playSource() to queue three buffers that will be played
  sequentially.


  \enum FaunCommand
  Commands used for faun_control().

  \var FaunCommand::FC_START
  Start playing from the beginning of the source buffer or stream.

  \var FaunCommand::FC_STOP
  Halt playback immediately.

  \var FaunCommand::FC_RESUME
  Continue playback from the point when FC_STOP was last used.

  \var FaunCommand::FC_FADE_OUT
  Fade volume to zero from the current play position over the
  FAUN_FADE_PERIOD.


  \enum FaunPlayMode
  Playback mode options for faun_playSource(), faun_playStream(), &
  faun_playStreamPart().

  \var FaunPlayMode::FAUN_PLAY_ONCE
  Used to initiate playback of a source or stream a single time.

  \var FaunPlayMode::FAUN_PLAY_LOOP
  Used to initiate playback of a source or stream and repeat it forever.

  \var FaunPlayMode::FAUN_PLAY_FADE_IN
  Increase gain from 0.0 gradually when playing begins.
  The target gain is set by FAUN_VOLUME and the fade period is set by
  FAUN_FADE_PERIOD.

  \var FaunPlayMode::FAUN_PLAY_FADE_OUT
  Decreases gain to 0.0 gradually just before the source or stream ends.
  The fade period is set by FAUN_FADE_PERIOD.

  \var FaunPlayMode::FAUN_PLAY_FADE
  Used to set both FAUN_PLAY_FADE_IN & FAUN_PLAY_FADE_OUT.

  \var FaunPlayMode::FAUN_SIGNAL_DONE
  Used to generate a FaunSignal when the source or stream is finished playing.
  If the source was started with FAUN_PLAY_LOOP then this signal will never
  occur.


  \enum FaunParameter
  Parameters are used to modify playback of a source or stream.
  They are modified using faun_setParameter().

  \var FaunParameter::FAUN_VOLUME
  The value ranges from 0.0 to 1.0.  The default value is 1.0.

  \var FaunParameter::FAUN_PAN
  The value ranges from -1.0 to 1.0.  The default value of 0.0 plays both
  stereo channels at full gain.

  \var FaunParameter::FAUN_FADE_PERIOD
  Duration in seconds for fading in & out.  The default value is 1.5 seconds.

  \var FaunParameter::FAUN_END_TIME
  Used to end playback of a source or stream before the buffer or stream file
  ends.  The value is the number of seconds from the start when the sound will
  be stopped.


  \struct FaunSignal
  This struct is used for faun_pollSignals() & faun_waitSignal().

  \var FaunSignal::id
  This is the index of the source or stream generating the signal.

  \var FaunSignal::signal
  This is the FaunPlayMode event (FAUN_SIGNAL_DONE, FAUN_SIGNAL_PROG)
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
  \param appName        Program identifier for networked audio systems.

  \returns Error string or NULL if successful.
*/
const char* faun_startup(int bufferLimit, int sourceLimit, int streamLimit,
                        const char* appName)
{
    const char* error;
    int i;

    if (! appName)
        appName = "Faun Audio";

    _errStream = stderr;
    if ((error = sysaudio_open(appName)))
        return error;

    _bufferLimit = bufferLimit = limitU(bufferLimit, BUFFER_MAX);
    _sourceLimit = sourceLimit = limitU(sourceLimit, SOURCE_MAX);
    _streamLimit = streamLimit = limitU(streamLimit, STREAM_MAX);

    i = bufferLimit * sizeof(FaunBuffer) +
        sourceLimit * sizeof(FaunSource) +
        streamLimit * sizeof(StreamOV);
    _abuffer = (FaunBuffer*) malloc(i);
    if (! _abuffer) {
        sysaudio_close();
        return "No memory for arrays";
    }

    _asource = (FaunSource*) (_abuffer + bufferLimit);
    _stream  = (StreamOV*)  (_asource + _sourceLimit);

    memset(_abuffer, 0, bufferLimit * sizeof(FaunBuffer));
    for (i = 0; i < sourceLimit; ++i)
        faun_sourceInit(_asource + i, i);
    for (i = 0; i < streamLimit; ++i)
        stream_init(_stream + i, sourceLimit + i);

    faun_allocBufferSamples(&_voice.mix, FAUN_F32, FAUN_CHAN_2, 44100,
                            44100 / UPDATE_HZ);

    if ((error = sysaudio_allocVoice(&_voice, UPDATE_HZ, appName))) {
        sysaudio_close();
        return error;
    }

    _audioUp = AUDIO_UP;


    // Start audioThread.

    if (! (_voice.cmd = tmsg_create(MSG_SIZE, 32))) {
        error = "Command port create failed";
        goto thread_fail0;
    }

    if (! (_voice.sig = tmsg_create(SIG_SIZE, 32))) {
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
  Redirect error messages from stderr.  Pass NULL to reset to stderr.
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
                (FAUN_VOLUME, FAUN_PAN, FAUN_FADE_PERIOD, FAUN_END_TIME).
  \param value  Value assigned to param.
*/
void faun_setParameter(int si, int count, uint8_t param, float value)
{
    if( _audioUp && param < FAUN_PARAM_COUNT)
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
  Execute Faun program.

  The bytecode program must be terminated by FO_END.

  \param bytecode   FuanOpcode instructions and data.
  \param len        Byte length of bytecode.
*/
void faun_program(const uint8_t* bytecode, int len)
{
    if( _audioUp )
    {
        uint8_t cmd[MSG_SIZE];
        int clen;

        cmd[0] = CMD_PROGRAM;

        while (len > 0)
        {
            clen = (len > (MSG_SIZE-2)) ? MSG_SIZE-2 : len;
            len -= clen;
            cmd[1] = clen;
            memcpy(cmd+2, bytecode, clen);
            bytecode += clen;
            tmsg_push(_voice.cmd, &cmd);
        }
    }
}


/**
  Load a file into a PCM buffer.

  \param bi       Buffer index.
  \param file     Path to audio file.
  \param offset   Byte offset to the start of data in the file.
  \param size     Bytes to read from file. Pass zero to read to the file end.

  \return Duration in seconds or zero upon failure.
*/
float faun_loadBuffer(int bi, const char* file, uint32_t offset, uint32_t size)
{
    if( _audioUp && bi < _bufferLimit )
    {
        FaunBuffer buf;

        // Load buffer in user thread.
        buf.sample.ptr = NULL;
        if (cmd_bufferFile(&buf, file, offset, size)) {
            uint8_t cmd[MSG_SIZE];

            cmd[0] = CMD_SET_BUFFER;
            cmd[1] = bi;
            memcpy(cmd+2, &buf, 16);
            tmsg_push(_voice.cmd, cmd);

            return (float) buf.used / (float) buf.rate;
        }
    }
    return 0.0f;
}


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


/**
  Begin playback of a buffer from a source.

  \param si     Source index.
  \param bi     Buffer indices.  Use the FAUN_PAIR() & FAUN_TRIO() macros to
                queue two or three buffers.
  \param mode   The FaunPlayMode (FAUN_PLAY_ONCE or FAUN_PLAY_LOOP).
*/
void faun_playSource(int si, int bi, int mode)
{
    if( _audioUp )
    {
        CommandA cmd;
        cmd.op     = CMD_PLAY_SOURCE;
        cmd.select = si;
        cmd.ext    = mode;
        cmd.arg.u32[0] = bi;
        faun_command(&cmd, 8);
    }
}


/**
  Open a file and optionally begin streaming.

  Reading can be limited to a specific chunk of the file if the size argument
  is non-zero.

  To begin playback the mode must include either FAUN_PLAY_ONCE or
  FAUN_PLAY_LOOP.  If it does not, then a FAUN_START command or a call to
  faun_playStreamPart() must be used to initiate play.

  \param si     Stream index.
  \param file   File path.
  \param offset Byte offset to start of stream data in file.
  \param size   Byte size of stream data in file, or zero if the entire file
                is to be used.
  \param mode   The FaunPlayMode (FAUN_PLAY_ONCE, FAUN_PLAY_LOOP, etc.).
*/
void faun_playStream(int si, const char* file, uint32_t offset, uint32_t size,
                     int mode)
{
    if( _audioUp )
    {
        FileChunk fc[2];
        fc[1].cfile = fopen(file, "rb");
        if (fc[1].cfile)
        {
            // Put command header into fc[0] area.
            uint8_t* cmd = (uint8_t*) (fc+1);
            cmd -= 4;
            cmd[0] = CMD_OPEN_STREAM;
            cmd[1] = si;
            *((uint16_t*) (cmd+2)) = mode;

            fc[1].offset = offset;
            fc[1].size   = size;

            tmsg_push(_voice.cmd, cmd);
        }
        else
            fprintf(_errStream, "Faun playStream cannot open \"%s\"\n", file);
    }
}


/**
  Begin playing a segment from a stream.

  The stream must have been previously initialized by faun_playStream().

  \param si         Stream index.
  \param start      Playback start position (in seconds).
  \param duration   Time to play (in seconds).
  \param mode       FaunPlayMode (FAUN_PLAY_ONCE or FAUN_PLAY_LOOP).
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
