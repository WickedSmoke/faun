old: 0,1,5
new: 0,2,0
files: [
    %Makefile           ["faun.so.$v"]
    %project.b          ["%faun $c"]
    %dist/cbuild        ["VERS=$v"]
    %dist/Doxyfile      ["= $v"]
    %dist/faun.spec     ["Version: $v"]
    %jni/build-sdk.sh   ["-$v"]
    /* */
    %faun.h [
        {FAUN_VERSION_STR    "$v"}
        {FAUN_VERSION        0x0$m0$i0$r}
    ]
]
