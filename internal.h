#include <stdint.h>
#include "os_thread.h"
#include "tmsg.h"

enum FaunSampleFormat {
    FAUN_U8,
    FAUN_S16,
    FAUN_S24,
    FAUN_F32,
    FAUN_FORMAT_COUNT
};

#define faun_channelCount(layout)   layout

// Speaker configuration (only 1 or 2 for now).
enum FaunChannelLayout {
   FAUN_CHAN_1 = 1,
   FAUN_CHAN_2,
   FAUN_CHAN_3,
   FAUN_CHAN_4,
   FAUN_CHAN_5_1,
   FAUN_CHAN_6_1,
   FAUN_CHAN_7_1
};

typedef struct {
    union {
        char*    c;
        uint8_t* u8;
        int16_t* s16;
        int32_t* s24;
        float*   f32;
        void*    ptr;
    } sample;
    uint32_t    avail;          // Number of frames allocated
    uint32_t    used;           // Number of frames set
    uint32_t    rate;           // Sample rate
    uint16_t    format;         // FaunSampleFormat
    uint16_t    chanLayout;     // FaunChannelLayout
}
FaunBuffer;

// A voice structure mixes all sources for the system/hardware voice.
typedef struct {
   FaunBuffer mix;
   struct MsgPort* cmd;
   struct MsgPort* sig;
   pthread_t    thread;
   void*        backend;
   uint32_t     updateHz;
#ifdef ANDROID
   uint32_t     frameBytes;
#endif
}
FaunVoice;

void faun_reserve(FaunBuffer* buf, int frames);
