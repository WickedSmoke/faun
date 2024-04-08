#!boron
; Faun bytecode compiler

faun-compile: func [blk block!] [
    bc: make binary! 48
    ifn parse blk [some [
        tok:
        'wait double!   (appair bc 1 to-int mul 10.0 second tok)
      | 'si int!        (appair bc 2 second tok)
      | 'queue int!     (appair bc 3 second tok)
      | 'play int!      (append bc reduce [4 second tok 0x1])
      | 'stream         (append bc 5)
      | 'stream-loop    (append bc 6)
      | 'vol double!    (appair bc 7 to-int mul 255.0 second tok)
      | 'fade-in        (append bc 13)
      | 'fade-out       (append bc 14)
      | 'signal         (append bc 15)
      | 'capture        (append bc 16)
      | path! int! (
            b: to-block first tok
            forall b [
                switch first b [
                    'play     [mode: 1]     ; Must be first!
                    'loop     [mode: or 2 and mode complement 1]
                    'fade-in  [mode: or mode 0x10]
                    'fade-out [mode: or mode 0x20]
                    'fade     [mode: or mode 0x30]
                    'sig-done [mode: or mode 0x40]
                    [error "Invalid play option"]
                ]
            ]
            append bc reduce [4 second tok mode]
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
            4 [prin " pb" ++ bc prin first bc ++ bc
                                prin ' ' prin to-hex first bc]
            5 [prin " po"]
            6 [prin " pl"]
            7 [prin " vo" ++ bc prin first bc]
           13 [prin " fi"]
           14 [prin " fo"]
           15 [prin " sg"]
           16 [prin " ca"]
              [prin "<?>"]
        ]
        ++ bc
    ]
]

probe code: faun-compile load first args
bc-arguments code
