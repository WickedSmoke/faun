#include <stdio.h>
#include "faun.h"

#define SRC_LIMIT   8
#define MUSIC_ID    SRC_LIMIT

int main()
{
    const char* err;
    FaunSignal sig;

    err = faun_startup(32, SRC_LIMIT, 2, 0, "Faun Example");
    if (err) {
        printf("%s\n", err);
        return 1;
    }

    // Start some music.
    faun_playStream(MUSIC_ID, "data/vintage_education.ogg", 0, 0,
                    FAUN_PLAY_ONCE | FAUN_SIGNAL_DONE);

    // Load and play a sound.
    faun_loadBuffer(0, "data/sa_enchant.rfx", 0, 0);
    faun_playSource(0, 0, FAUN_PLAY_ONCE);

    // Wait for music to finish.
    faun_waitSignal(&sig);

    faun_shutdown();
    return 0;
}
