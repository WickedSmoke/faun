#if USE_FLAC == 2
#include "flac.c"

static const char* foxenFlacDecode(FILE* fp, uint32_t size, FaunBuffer* buf,
                                   const void* preRead, size_t preReadLen)
{
    const char* error = NULL;
    const int bufSize = 256;
    const int decodeSize = 512;
    uint8_t* readBuf;
    int32_t* decodeBuf;
    size_t toRead, n;
    uint32_t inUsed, procLen;
    uint32_t bufPos = 0;
    //uint32_t frate;
    uint32_t fchannels;
    uint32_t frames = 0;
    float* pcmOut = NULL;
    fx_flac_state_t fstate;
    fx_flac_t* flac = FX_FLAC_ALLOC_SUBSET_FORMAT_DAT();

    decodeBuf = (int32_t*) malloc(decodeSize*sizeof(int32_t) + bufSize);
    readBuf   = (uint8_t*) (decodeBuf + decodeSize);

    memcpy(readBuf, preRead, preReadLen);
    bufPos = preReadLen;

    if (size)
        size -= preReadLen;
    else
        size = UINT32_MAX;

    while (1) {
        if (size) {
            toRead = bufSize - bufPos;
            if (toRead > 0) {
                if (toRead > size)
                    toRead = size;
                n = fread(readBuf + bufPos, 1, toRead, fp);
                if (n == 0) {
                    size = 0;       // Stop reading but continue decoding.
                } else {
                    size -= n;
                    bufPos += n;    // Advance the write cursor
                }
            }
        }

        inUsed  = bufPos;
        procLen = decodeSize;
        fstate = fx_flac_process(flac, readBuf, &inUsed,
                                       decodeBuf, &procLen);
        if (fstate == FLAC_ERR) {
            error = "FLAC decode failed";
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
                error = "FLAC total samples is unknown";
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

        n = bufPos - inUsed;
        if (n == 0) {
            // Exit loop when both decoding & reading are done.
            if (procLen == 0 && size == 0)
                break;
        } else if (inUsed) {
            // Move unprocessed bytes to the beginning of readBuf.
            //printf("KR unproc %ld pos:%d used:%d\n", n, bufPos, inUsed);
            memmove(readBuf, readBuf + inUsed, n);
        }
        bufPos = n;
    }
    buf->used = frames;

    free(decodeBuf);
    free(flac);
    return error;
}

#else
#include <FLAC/stream_decoder.h>

typedef struct {
    FaunBuffer* buf;
    float* pcmOut;

    FILE* fp;
    uint32_t length;

    uint64_t totalSamples;
    uint32_t rate;
    uint16_t channels;
    uint16_t bitsPerSample;
} FlacReader;

static FLAC__StreamDecoderReadStatus flacRead(const FLAC__StreamDecoder* fdec,
        FLAC__byte buffer[], size_t* bytes, void* client_data)
{
    FlacReader* rd = (FlacReader*) client_data;
    size_t len = *bytes;
    (void) fdec;

    if (len > rd->length)
        len = rd->length;
    len = fread(buffer, 1, len, rd->fp);
    rd->length -= len;
    *bytes = len;

    if (len > 0)
        return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    if (ferror(rd->fp))
        return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
}

static void flacMetadata(const FLAC__StreamDecoder* fdec,
                         const FLAC__StreamMetadata* metadata, void* client_data)
{
    FlacReader* rd = (FlacReader*) client_data;
    (void)fdec;

    if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
        if (rd->pcmOut) {
            fprintf(_errStream, "FLAC stream_info repeated\n");
            return;
        }

        rd->totalSamples  = metadata->data.stream_info.total_samples;
        rd->rate          = metadata->data.stream_info.sample_rate;
        rd->channels      = metadata->data.stream_info.channels;
        rd->bitsPerSample = metadata->data.stream_info.bits_per_sample;
#if 0
        printf("FLAC meta samples:%lu rate:%d chan:%d bps:%d\n",
               rd->totalSamples, rd->rate, rd->channels, rd->bitsPerSample);
#endif

        if (rd->rate != 44100 && rd->rate != 22050) {
            fprintf(_errStream, "FLAC sample rate %d not handled\n", rd->rate);
            return;
        }

        if (rd->bitsPerSample < 16) {
            fprintf(_errStream, "FLAC bps %d not handled\n", rd->bitsPerSample);
            return;
        }

        if (rd->totalSamples) {
            _allocBufferVoice(rd->buf, rd->totalSamples);
            rd->pcmOut = rd->buf->sample.f32;
        }
    }
}

static FLAC__StreamDecoderWriteStatus flacWrite(const FLAC__StreamDecoder* fdec,
        const FLAC__Frame* frame, const FLAC__int32* const buffer[], void* client_data)
{
    FlacReader* rd = (FlacReader*) client_data;
    float* pcmOut = rd->pcmOut;
    const FLAC__int32* chan0 = buffer[0];
    const FLAC__int32* chan1;
    size_t i, procLen = frame->header.blocksize;
    int shift = rd->bitsPerSample - 16;
    float ds0, ds1;
    (void) fdec;

    if (! pcmOut)
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;

    if (rd->rate == 22050) {
        if (rd->channels == 1) {
            for (i = 0; i < procLen; i++) {
                ds0 = (chan0[i] >> shift) / 32767.0f;
                *pcmOut++ = ds0;
                *pcmOut++ = ds0;
                *pcmOut++ = ds0;
                *pcmOut++ = ds0;
            }
        } else {
            chan1 = buffer[1];
            for (i = 0; i < procLen; i++) {
                ds0 = (chan0[i] >> shift) / 32767.0f;
                ds1 = (chan1[i] >> shift) / 32767.0f;
                *pcmOut++ = ds0;
                *pcmOut++ = ds1;
                *pcmOut++ = ds0;
                *pcmOut++ = ds1;
            }
        }
    } else {
        if (rd->channels == 1) {
            for (i = 0; i < procLen; i++) {
                ds0 = (chan0[i] >> shift) / 32767.0f;
                *pcmOut++ = ds0;
                *pcmOut++ = ds0;
            }
        } else {
            chan1 = buffer[1];
            for (i = 0; i < procLen; i++) {
                *pcmOut++ = (chan0[i] >> shift) / 32767.0f;
                *pcmOut++ = (chan1[i] >> shift) / 32767.0f;
            }
        }
    }

    rd->pcmOut = pcmOut;
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void flacError(const FLAC__StreamDecoder* fdec,
                      FLAC__StreamDecoderErrorStatus status, void* client_data)
{
    (void) fdec;
    (void) client_data;
    fprintf(_errStream, "FLAC decode error: %s\n",
            FLAC__StreamDecoderErrorStatusString[status]);
}

static const char* libFlacDecode(FILE* fp, uint32_t size, FaunBuffer* buf)
{
    FLAC__StreamDecoder* dec;
    const char* error = NULL;
    FlacReader fr;

    fr.buf = buf;
    fr.pcmOut = NULL;
    fr.fp = fp;
    fr.length = size ? size : UINT32_MAX;
    fr.totalSamples = 0;
    fr.rate = 0;

    dec = FLAC__stream_decoder_new();

    if (FLAC__stream_decoder_init_stream(dec, flacRead, NULL, NULL, NULL, NULL,
                                         flacWrite, flacMetadata, flacError, &fr)
        == FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        if (FLAC__stream_decoder_process_until_end_of_stream(dec))
            buf->used = fr.totalSamples;
        else
            error = "FLAC process failed";
    } else
        error = "FLAC decoder init failed";

    FLAC__stream_decoder_delete(dec);
    return error;
}
#endif
