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
