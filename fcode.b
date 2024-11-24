#!boron
; Faun bytecode compiler

unit-val: func [n double!] [to-int mul 255.0 n]
vol-pair: func [bin volL volR] [appair bin unit-val volL unit-val volR]

faun-compile: func [blk block!] [
    bc: make binary! 48
    ifn parse blk [some [
        tok:
        'wait double!   (appair bc 1 to-int mul 10.0 second tok)
      | 'si int!        (appair bc 2 second tok)
      | 'queue int!     (appair bc 3 second tok)
      | 'play int!      (append bc reduce [4 second tok 0x1])
      | 'stream         (appair bc 5 1)
     ;| 'reserved       (append bc 6)
      | 'set-vol  double! (appair bc 7 unit-val second tok)
      | 'set-fade double! (appair bc 8 to-int mul 10.0 second tok)
      | 'fade-in        (append bc 12)
      | 'fade-out       (append bc 13)
      | 'vol-lr double! double! (append bc 14 vol-pair bc second tok third tok)
      | 'pan    double! double! (append bc 15 vol-pair bc second tok third tok)
      | 'signal         (append bc 16)
      | 'capture        (append bc 17)
      | path! opt int! (
            b: to-block first tok
            stype: first b
            forall b [
                switch first b [
                    'play     [mode: 1]     ; Play or stream must be first!
                    'stream   [mode: 1]
                    'loop     [mode: or 2 and mode complement 1]
                    'fade-in  [mode: or mode 0x10]
                    'fade-out [mode: or mode 0x20]
                    'fade     [mode: or mode 0x30]
                    'sig-done [mode: or mode 0x40]
                    [error "Invalid play option"]
                ]
            ]
            case [
                all [eq? 'play stype int? second tok] [
                    append bc reduce [4 second tok mode]
                ]
                eq? 'stream stype [
                    appair bc 5 mode
                ]
                [error "Invalid play path!"]
            ]
        )
    ]]
        [error "Invalid Faun program"]
    append bc 0
]

bc-arguments: func [bc binary!] [
    while [not tail? bc] [
        switch first bc [
            0 [print " en"]
            1 [prin " wa" ++ bc prin first bc]
            2 [prin " so" ++ bc prin first bc]
            3 [prin " qu" ++ bc prin first bc]
            4 [prin " pb" prin [second bc to-hex third bc] bc: skip bc 2]
            5 [prin " ss" ++ bc prin first bc]
           ;6 [prin " "]
            7 [prin " vo" ++ bc prin first bc]
            8 [prin " fp" ++ bc prin first bc]
           12 [prin " fi"]
           13 [prin " fo"]
           14 [prin " vc" prin [second bc third bc] bc: skip bc 2]
           15 [prin " pa" prin [second bc third bc] bc: skip bc 2]
           16 [prin " sg"]
           17 [prin " ca"]
              [prin "<?>"]
        ]
        ++ bc
    ]
]

probe code: faun-compile load first args
bc-arguments code
