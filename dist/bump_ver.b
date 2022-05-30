old: 0,1,0
new: 0,1,1
files: [
    %Makefile           ["faun.so.$v"]
    %project.b          ["%faun $c"]
    %dist/cbuild        ["-$v"]
    %dist/download.md   [{## v$v Static} {ic-$v}]
   ;%faun.h [
   ;    {FAUN_VERSION_STR    "$v"}
   ;    {FAUN_VERSION        0x0$m0$i0$r}
   ;]
]
