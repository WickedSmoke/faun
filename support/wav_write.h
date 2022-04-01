#ifndef WAVE_WRITE_H
#define WAVE_WRITE_H

#ifdef __cplusplus
extern "C" {
#endif

FILE* wav_open(const char* filename, int rate, int bitsPerSample, int channels);
void  wav_close(FILE*);
void  wav_write(FILE*, float* samples, int count);

#ifdef __cplusplus
}
#endif

#endif
