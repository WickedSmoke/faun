#ifndef FAUN_H
#define FAUN_H
/*  ___________
    \_   _____/____   __ __  ____
     |    __) \__  \ |  |  \/    \
     |    \    / __ \|  |  /   |  \
     \__  /   (____  /____/|___|  /
        \/         \/           \/

  Faun - A high-level C audio library

  Copyright (c) 2022-2024 Karl Robillard
  This code may be used under the terms of the MIT license (see faun.c).
*/

#include <stdint.h>
#include <stdio.h>

#define FAUN_VERSION_STR    "0.2.0"
#define FAUN_VERSION        0x000200

enum FaunCommand {
    FC_START,
    FC_STOP,
    FC_RESUME,
    FC_FADE_OUT,
    FC_COUNT
};

enum FaunOpcode {
    FO_END,
    FO_WAIT,            // 1/10 second units
    FO_SOURCE,          // Source #
    FO_QUEUE,           // Buffer #
    FO_PLAY_BUF,        // Buffer #, mode
    FO_START_STREAM,    // mode
    FO_RESERVED0,
    FO_SET_VOL,         // Unit value
    FO_SET_FADE,        // 1/10 second units
    FO_SET_END,         // 1/10 second units
    FO_LOOP_ON,
    FO_LOOP_OFF,
    FO_FADE_IN,
    FO_FADE_OUT,
    FO_VOL_LR,          // L volume, R volume
    FO_PAN,             // L target, R target
    FO_SIGNAL,
    FO_CAPTURE,
    /*
    FO_SET_VOL_f,       // float argN
    FO_SET_FADE_f,      // float argN
    FO_VOL_LR_f,        // float argN
    FO_PAN_f,           // float argN, float argN
    */
    FO_COUNT
};

#define FAUN_PROGRAM_MAX    64

enum FaunFormat {
    FAUN_FMT_S16    = 1,
    FAUN_FMT_F32    = 2,
    FAUN_FMT_MONO   = 0,
    FAUN_FMT_STEREO = 8,
    FAUN_FMT_22050  = 0x10,
    FAUN_FMT_44100  = 0x20
};

enum FaunPlayMode {
    FAUN_PLAY_ONCE      = 0x0001,
    FAUN_PLAY_LOOP      = 0x0002,
    FAUN_PLAY_FADE_IN   = 0x0010,
    FAUN_PLAY_FADE_OUT  = 0x0020,
    FAUN_SIGNAL_DONE    = 0x0040,
    FAUN_SIGNAL_PROG    = 0x0080,
    FAUN_PLAY_FADE      = FAUN_PLAY_FADE_IN | FAUN_PLAY_FADE_OUT
};

#define FAUN_PAIR(a,b)      (((b+1) << 10) | a)
#define FAUN_TRIO(a,b,c)    (((c+1) << 20) | ((b+1) << 10) | a)
#define FAUN_PID_SOURCE(pid) (pid & 0xff)

enum FaunParameter {
    FAUN_VOLUME,
    FAUN_VOLUME_APPLY,
    FAUN_FADE_PERIOD,
    FAUN_END_TIME,
    FAUN_PARAM_COUNT
};

typedef struct {
    uint32_t id;
    uint16_t signal;
}
FaunSignal;

#ifdef __cplusplus
extern "C" {
#endif

const char* faun_startup(int bufferLimit, int sourceLimit,
                         int streamLimit, int progLimit, const char* appName);
void faun_shutdown();
void faun_suspend(int halt);
void faun_setErrorStream(FILE*);
int  faun_pollSignals(FaunSignal* sigbuf, int count);
void faun_waitSignal(FaunSignal* sigbuf);
void faun_control(int si, int count, int command);
void faun_setParameter(int si, int count, uint8_t param, float value);
void faun_pan(int si, float finalVolL, float finalVolR, float period);
void faun_program(int ei, const uint8_t* bytecode, int len);

float faun_loadBuffer(int bi, const char* file, uint32_t offset, uint32_t size);
float faun_loadBufferF(int bi, FILE* file, uint32_t size);
float faun_loadBufferPcm(int bi, int format, const void* samples,
                         uint32_t frames);
float faun_loadBufferSfx(int bi, const void* sfxParam);
void  faun_freeBuffers(int bi, int count);
uint32_t faun_playSource(int si, int bi, int mode);
uint32_t faun_playSourceVol(int si, int bi, int mode, float volL, float volR);

uint32_t faun_playStream(int si, const char* file, uint32_t offset,
                         uint32_t size, int mode);
void faun_playStreamPart(int si, double start, double duration, int mode);
int  faun_isPlaying(uint32_t pid);

#ifdef __cplusplus
}
#endif

#endif  // FAUN_H
