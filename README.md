Faun Audio Library
==================

Faun is a high-level C API for playback of sound & music in games & demos.
It is a modestly sized library designed to use pre-packaged audio and is not
intended for synthesizer or audio manipulation applications.  The shared
library is about half the size the SDL 2 & Allegro 5 mixer libraries.

The following audio formats can be played:
  - FLAC
  - Ogg Vorbis
  - [rFX][] (sfxr synthesizer)
  - WAVE

The following operating systems are supported:
  - Android (via AAudio)
  - Linux (via PulseAudio)
  - Windows (via Windows Audio Session)

Features include:
  - Support for audio embedded in larger files.
  - Playback of stream fragments.
  - Fading volume in & out.
  - Source playback from a queue of buffers.
  - Signaling when a sound is finished playing.
  - A bytecode language for running simple playback sequences.

At this time the library does not support:
  - Sample rates other than 44100 & 22050 Hz.
  - More than two channels.
  - 3D audio.
  - Feeding PCM data directly from memory.

> _**NOTE:**_ This library is under development and the API is subject to
> change.

Example usage:

    #include <faun.h>

    int main() {
        FaunSignal sig;

        faun_startup(64, 8, 2, 0, "Faun Test");

        // Start some music.
        faun_playStream(8, "path/to/music.ogg", 0, 0,
                        FAUN_PLAY_ONCE | FAUN_SIGNAL_DONE);

        // Load and play a sound.
        faun_loadBuffer(0, "path/to/sound.flac", 0, 0);
        faun_playSource(0, 0, FAUN_PLAY_ONCE);

        // Wait for music to finish.
        faun_waitSignal(&sig);

        faun_shutdown();
    }

Resource Management
-------------------

**Sources** are used to play audio samples completely stored in memory
**buffers**.  **Streams** are used to play longer sounds by continually
decoding small parts of the file into memory.  While the stream implemention
uses sources & buffers internally, for the API user streams are treated as
separate resources.

The maximum number of source buffers, sources, & streams that can be used by
a program is set once using `faun_startup()`.  This reserves slots which are
specified by index (zero to limit-1) which may be used as desired.

### Sources and Buffers

Before a source can play, buffers must be loaded using `faun_loadBuffer()`.
The `faun_playSource()` function can then be used to play 1-3 buffers in series
as a single sound.

### Streams

The `faun_playStream()` & `faun_playStreamPart()` functions are used to play
streams.

> _**NOTE:**_ Currently only Ogg Vorbis files can be streamed.


[rFX]: https://raylibtech.itch.io/rfxgen
