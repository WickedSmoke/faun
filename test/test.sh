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

# Audio Files
ST_TO=data/townes.ogg
ST_LB=data/voice-lb.ogg
SO_G=data/gate_open.ogg
SO_E=data/evade_dos.ogg
SO_F=data/TMP_Red_Alert_Klaxon.flac
SO_R=data/si_blink.rfx
SO_L=data/thx-lfreq.flac


capture  1 t01-so "-b0 $SO_G -b1 $SO_E -p0 0 401 /2 -p1 1 1 /2 -p1 1 1 -W"
capture  2 t02-st-fade "-a8 fade 2.0 -m8 501 $ST_TO /4 -c8 fade_out -W"
capture  3 t03-st-segment "-m8 0 $ST_LB -s 1 0.0 2.856054 /4 -s 1 19.882448 3.297234 /4 -s 1 19.882448 3.297234 /4 -s 401 0.0 2.856054 -W"
capture  4 t04-so-sfx  "-b0 $SO_R -p0 0 401 -W"
capture  5 t05-so-flac "-b0 $SO_F -p0 0 401 -W"
capture  6 t06-so-end  "-b0 $SO_L -p0 0 401 -a0 end 3.0 -W"
capture  7 t07-st-restart "-m8 1 $ST_TO  /3 -m8 401 $SO_G -a8 end 2.0 -W"
capture  8 t08-suspend "-m8 401 $ST_TO -a8 end 6.0 /3 -z /4 -z -W"
capture  9 t09-queue   "-b0 $SO_E -b1 $SO_F -b2 $SO_R -p0 300800 401 -W"
capture 10 t10-so-fade "-b0 $SO_F -p0 0 701 -W"
capture 11 t11-so-vol  "-b0 $SO_F -a0 vol 0.1 -p0 400 401 /2 -a0 vol 0.5 /2 -a0 vol 1.0 /2 -a0 vol 0.2 -W"
capture 12 t12-st-vol  "-a8 vol 0.1 -m8 401 $ST_TO /2 -a8 vol 0.5 /2 -a8 vol 1.0 /2 -a8 vol 0.2 -c8 fade_out -W"
capture 13 t13-s0-loop "-b0 $SO_E -p0 0 481 /1 -c0 fade_out -W"

report
