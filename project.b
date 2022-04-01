options [
    flac:   true    "Use foxenflac loader (GPL)"
    static: false   "Build static library"
]

libfaun: [
    cflags "-DCAPTURE -DUSE_SFX_GEN"
    if flac [cflags "-DUSE_FLAC"]
    include_from %support
    if msvc [include_from %../usr/include]
    sources [
        %support/tmsg.c
        %faun.c
    ]
]

faun-dep: [
    linux [libs [%pulse-simple %pulse %vorbis %vorbisfile %pthread %m]]
    win32 [
        either msvc
            [libs_from %../usr/lib [%vorbis %vorbisfile %ogg]]
            [libs [%vorbis %vorbisfile]]
       ;libs [%dsound %dxguid %uuid %user32]
        libs [%ole32 %user32]
    ]
]

flink: [
    include_from %.
    libs_from %. %faun
]

either static [
    lib %faun libfaun
    faun-link: does append flink faun-dep
][
    shlib [%faun 0,1,0] append append libfaun faun-dep [
        win32 [lflags either msvc "/def:faun.def" "faun.def"]
    ]
    faun-link: does flink
]

exe %faun_test [
    console
    sources [%faun_test.c]
    faun-link
]

exe %basic [
    console
    sources [%example/basic.c]
    faun-link
]

dist [
    %faun.def
    %support/cpuCounter.h
    %pulseaudio.c
    %wasapi.c
]
