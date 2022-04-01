#include <stdint.h>
#include <stdio.h>

/*
 * Save PCM data to WAVE file.
 * Return error message or NULL if successful.
 */
FILE* wav_open(const char* filename, int rate, int bitsPerSample, int channels)
{
    uint32_t dword;
    uint16_t word;
    uint32_t bytesPerSec = rate * bitsPerSample/8 * channels;
    FILE* fp = fopen(filename, "wb");
    if(! fp)
        return NULL;

#define WRITE_ID(STR) \
    if (fwrite(STR, 4, 1, fp) != 1) \
        goto cleanup

#define WRITE_32(N) \
    dword = N; \
    fwrite(&dword, 1, 4, fp)

#define WRITE_16(N) \
    word = N; \
    fwrite(&word, 1, 2, fp)

    // Write WAVE header.
    WRITE_ID("RIFF");               // "RIFF"
    WRITE_32(36 /*+ dataSize*/);    // Remaining file size
    WRITE_ID("WAVE");               // "WAVE"

    WRITE_ID("fmt ");               // "fmt "
    WRITE_32(16);                   // Chunk size
    WRITE_16(1);                    // Compression code
    WRITE_16(channels);             // Channels
    WRITE_32(rate);                 // Sample rate
    WRITE_32(bytesPerSec);          // Bytes/sec
    WRITE_16(bitsPerSample/8);      // Block align
    WRITE_16(bitsPerSample);        // Bits per sample

    // Write sample data.
    WRITE_ID("data");               // "data"
    WRITE_32(0 /*dataSize*/);       // Chunk size
    return fp;

cleanup:
    fclose(fp);
    return NULL;
}

void wav_close(FILE* fp)
{
    uint32_t size = ftell(fp);
    size -= 8;
    fseek(fp, 4, SEEK_SET);
    fwrite(&size, 4, 1, fp);    // Remaining file size

    size -= 36;
    fseek(fp, 40, SEEK_SET);
    fwrite(&size, 4, 1, fp);    // Data chunk size

    fclose(fp);
}

void wav_write(FILE* fp, float* samples, int count)
{
    int16_t is;
    int i;
    for (i = 0; i < count; ++i) {
        is = (int16_t) (*samples++ * 32767.0f);
        fwrite(&is, 2, 1, fp);
    }
}
