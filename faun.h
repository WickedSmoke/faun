#ifndef FAUN_H
#define FAUN_H
/*  ___________
    \_   _____/____   __ __  ____
     |    __) \__  \ |  |  \/    \
     |    \    / __ \|  |  /   |  \
     \__  /   (____  /____/|___|  /
        \/         \/           \/

  Faun - A high-level C audio library

  Copyright (c) 2022 Karl Robillard
  This code may be used under the terms of the MIT license (see faun.c).
*/

#include <stdint.h>
#include <stdio.h>

#define FAUN_VERSION_STR    "0.1.0"
#define FAUN_VERSION        0x000100

enum FaunCommand {
    FC_START,
    FC_STOP,
    FC_RESUME,
    FC_FADE_OUT,
    FC_COUNT
};

enum FaunPlayMode {
    FAUN_PLAY_ONCE      = 0x0001,
    FAUN_PLAY_LOOP      = 0x0080,
    FAUN_PLAY_FADE_IN   = 0x0100,
    FAUN_PLAY_FADE_OUT  = 0x0200,
    FAUN_SIGNAL_DONE    = 0x0400,
    FAUN_SIGNAL_BUFFER  = 0x0800,
    FAUN_PLAY_FADE      = FAUN_PLAY_FADE_IN | FAUN_PLAY_FADE_OUT
};

#define FAUN_PAIR(a,b)      (((b+1) << 10) | a)
#define FAUN_TRIO(a,b,c)    (((c+1) << 20) | ((b+1) << 10) | a)

enum FaunParameter {
    FAUN_VOLUME,
    FAUN_PAN,
    FAUN_FADE_PERIOD,
    FAUN_END_TIME,
    FAUN_PARAM_COUNT
};

typedef struct {
    uint16_t id;
    uint16_t signal;
}
FaunSignal;

#ifdef __cplusplus
extern "C" {
#endif

const char* faun_startup(int bufferLimit, int sourceLimit,
                         int streamLimit, const char* appName);
void faun_shutdown();
void faun_suspend(int halt);
void faun_setErrorStream(FILE*);
int  faun_pollSignals(FaunSignal* sigbuf, int count);
void faun_waitSignal(FaunSignal* sigbuf);
void faun_control(int si, int count, int command);
//void faun_controlSeq(int si, int count, const uint8_t* commands);
void faun_setParameter(int si, int count, uint8_t param, float value);

float faun_loadBuffer(int bi, const char* file, uint32_t offset, uint32_t size);
void  faun_freeBuffers(int bi, int count);
void  faun_playSource(int si, int bi, int mode);

void faun_playStream(int si, const char* file, uint32_t offset,
                     uint32_t size, int mode);
void faun_playStreamPart(int si, double start, double duration, int mode);

#ifdef __cplusplus
}
#endif

#endif  // FAUN_H
