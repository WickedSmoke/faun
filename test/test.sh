# Faun Tests
# Must be run from the repository top level directory.

PROG=./faun_test
#PROG='wine ./faun_test.exe'

if [ ! -d test/good ]; then
    echo "test.sh must be invoked from the faun root directory."
    exit 2
fi

source test/azeroth.sh

# 1:test-id 2:output 3:args
capture() {
    if filtered $1 $(funcname); then return 0; fi
    if [ -n "$PRINT_CMD" ]; then
        echo "Test $1: $PROG $3"
        return 0
    fi
    announce $1 $(funcname) `basename "$2"`

    if [ -n "$VALGRIND_CMD" ]; then
        valgrind --leak-check=full $PROG $3 >/tmp/$2.vg 2>&1
		ES=$?
		if [[ $ES -ne 0 ]]; then
			fail valgrind
			return 1
		fi
		if [ -z "$KEEP_OUTPUT" ]; then
			rm /tmp/$2.vg
		fi
		pass
        return 0
    fi

	export FAUN_CAPTURE="/tmp/$2.wav"
    $PROG $3
    ES=$?
    if [[ $ES -ne 0 ]]; then
        fail "$PROG $ES"
        return 1
    fi

    if ! sha1sum --status -c test/good/$2; then
        fail checksum
        return 1
    fi

    if [ -z "$KEEP_OUTPUT" ]; then
        rm ${FAUN_CAPTURE}
    fi
    pass
}

# 1:test-id 2:name 3:args
listen() {
    if filtered $1 $(funcname); then return 0; fi
    if [ -n "$PRINT_CMD" ]; then
        echo "Test $1: $PROG $3"
        return 0
    fi
    announce $1 $(funcname) `basename "$2"`

    if [ -n "$VALGRIND_CMD" ]; then
        valgrind --leak-check=full $PROG $3 >/tmp/$2.vg 2>&1
		ES=$?
		if [[ $ES -ne 0 ]]; then
			fail valgrind
			return 1
		fi
		if [ -z "$KEEP_OUTPUT" ]; then
			rm /tmp/$2.vg
		fi
		pass
        return 0
    fi

    $PROG $3
    ES=$?
    if [[ $ES -ne 0 ]]; then
        fail "$PROG $ES"
        return 1
    fi
    pass
}

# 1:test-id 2:name 3:source-file
fcode() {
    if filtered $1 $(funcname); then return 0; fi
    if [ -n "$PRINT_CMD" ]; then
        echo "Test $1: boron fcode.b $3 >/tmp/$2"
        return 0
    fi
    announce $1 $(funcname) "$3"

    if [ -n "$VALGRIND_CMD" ]; then
        fail "N/A"
        return 1
    fi

    boron fcode.b "$3" >/tmp/$2
    ES=$?
    if [[ $ES -ne 0 ]]; then
        fail "fcode.b $ES"
        return 1
    fi

    if ! diff /tmp/$2 test/good/$2; then
        fail diff
        return 1
    fi

    if [ -z "$KEEP_OUTPUT" ]; then
        rm /tmp/$2
    fi
    pass
}


# Audio Files
ST_TO=data/townes.ogg
ST_LB=data/voice-lb.ogg
SO_G=data/gate_open.ogg
SO_E=data/evade_dos.ogg
SO_F=data/TMP_Red_Alert_Klaxon.flac
SO_H=data/Hyperdrive_Abort.ogg
SO_R=data/si_blink.rfx
SO_A=data/sa_alert.rfx
SO_N=data/sa_enchant.rfx
SO_L=data/thx-lfreq.flac


capture  1 t01-so "-b0 $SO_G -b1 $SO_E -o ca so0 pb0 41 wa20 so1 pb1 1 wa20 pb1 1 en -W"
capture  2 t02-st-fade "-m8 0 $ST_TO -o ca so8 ss52 wa40 fo en -W"
listen   3 t03-st-segment "-m8 0 $ST_LB -s 1 0.0 2.856054 /4 -s 1 19.882448 3.297234 /4 -s 1 19.882448 3.297234 /4 -s 41 0.0 2.856054 -W"
capture  4 t04-so-sfx  "-b0 $SO_R -o ca so0 pb0 41 en -W"
capture  5 t05-so-flac "-b0 $SO_F -o ca so0 pb0 41 en -W"
capture  6 t06-so-end  "-b0 $SO_L -o ca so0 pb0 41 ep30 en -W"
listen   7 t07-st-restart "-m8 1 $ST_TO /3 -m8 41 $SO_G -a8 end 2.0 -W"
listen   8 t08-suspend "-m8 41 $ST_TO -a8 end 6.0 /3 -z /4 -z -W"
capture  9 t09-queue   "-b0 $SO_E -b1 $SO_F -b2 $SO_R -o ca so0 pb0 41 qu1 qu2 en -W"
capture 10 t10-so-fade "-b0 $SO_F -o ca so0 pb0 71 en -W"
capture 11 t11-so-vol  "-b0 $SO_F -a0 vol 0.1 -o ca so0 pb0 41 qu0 wa20 vc128 128 wa20 vc255 255 wa20 vc51 51 en -W"
capture 12 t12-st-vol  "-m8 0 $ST_TO -o ca so8 ss42 wa20 vc128 128 wa20 vc255 255 wa20 vc51 51 fo en -W"
capture 13 t13-so-loop "-b0 $SO_E -o ca so0 pb0 41 ly wa10 fo en -W"
listen  14 t14-st-short "-m8 0 $ST_LB -s 41 15.377778 0.534 -W"
capture 15 t15-mix5    "-b0 $SO_F -b1 $SO_R -b2 $SO_N -b3 $SO_G -m8 0 $ST_TO -o ca so8 ss52 so0 vo128 pb3 1 so1 vo70 pb0 2 wa10 so2 pb1 2 so3 pb2 2 wa30 so8 fo en -W"
capture 16 t16-mix8    "-b0 $SO_F -b1 $SO_R -b2 $SO_N -b3 $SO_G -o ca so0 vo128 pb3 2 so1 vo70 pb0 2 so2 pb1 2 so3 pb2 2 wa20 so4 vo128 pb3 41 so5 vo70 pb0 2 so6 pb1 2 so7 pb2 2 en -W"
capture 17 t17-pan     "-b0 $SO_N -o ca so0 pb0 1 vc220 0 wa20 pb0 1 vc220 220 wa20 pb0 41 vc0 220 en -W"
capture 18 t18-st-pan  "-m8 0 $ST_TO -o ca so8 ss41 vc100 0 wa20 vc255 126 wa20 vc0 100 wa20 vc126 255 wa20 vc100 100 wa20 fo en -W"
capture 19 t19-st-mult "-m8 0 $ST_TO -m9 0 $ST_LB -m10 0 $SO_G -o ca so8 vo140 ss1 so9 ss1 so10 ss41 en -W"
capture 20 t20-22khz-ogg "-b0 $SO_H -o ca so0 pb0 41 en -W"
capture 21 t21-panf    "-b0 $SO_A -o ca so0 pb0 42 ep75 pa255 0 wa20 pa100 100 wa15 pa0 255 wa20 pa255 255 en -W"

fcode   30 t30-fc example/fcode01.b

report
