old: 0,1,2
new: 0,1,3
files: [
    %Makefile           ["faun.so.$v"]
    %project.b          ["%faun $c"]
    %dist/cbuild        ["-$v"]
    %jni/build-sdk.sh   ["-$v"]
   ;%faun.h [
   ;    {FAUN_VERSION_STR    "$v"}
   ;    {FAUN_VERSION        0x0$m0$i0$r}
   ;]
]
