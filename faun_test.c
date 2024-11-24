// cc -DUNIT_TEST test_stream.c -lopenal -lvorbis -lvorbisfile -lpthread
//
// Here is the basic usage to play an audio file and wait two seconds:
//  ./faun_test -b0 data/some_file.wav -p0 0 1 /2

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define sleep(S)	Sleep((S)*1000)
#else
#include <unistd.h>
#endif
#include "faun.h"

#define EX_NOINPUT  66  /* cannot open input */

extern void faun_closeOnSignal();


int hex(const char* str)
{
    return (int) strtol(str, NULL, 16);
}

int command(const char* str)
{
    static const char* cmdName[FC_COUNT] = {
        "start", "stop", "resume", "fade_out"
    };
    int i;
    for (i = 0; i < FC_COUNT; ++i) {
        if (strcmp(str, cmdName[i]) == 0)
            return i;
    }
    fprintf(stderr, "Invalid command: %s\n", str);
    return FC_COUNT;
}

int param(const char* str)
{
    static const char* paramName[FAUN_PARAM_COUNT] = {
        "vol", "fade", "end"
    };
    int i;
    for (i = 0; i < FAUN_PARAM_COUNT; ++i) {
        if (strcmp(str, paramName[i]) == 0)
            return i;
    }
    fprintf(stderr, "Invalid parameter: %s\n", str);
    return FAUN_PARAM_COUNT;
}

int main(int argc, char** argv)
{
    const char* error;
    const char* arg;
    FaunSignal sig;
    uint8_t program[FAUN_PROGRAM_MAX];
    uint8_t* pc = program;
    int opcodeMode = 0;
    int i, ch;
    int si = 0;
    int enabled = 1;
    uint32_t offset = 0;
    uint32_t size = 0;

    if ((error = faun_startup(16, 8, 3, 1, "Faun Test"))) {
        fprintf(stderr, "faun_startup: %s\n", error);
        return 1;
    }

#define INC_ARG     if (++i >= argc) break

#define PUSH_OP_ARG(opcode) \
    *pc++ = opcode; \
    *pc++ = atoi(argv[i] + 2)

    for (i = 1; i < argc; ++i)
    {
        arg = argv[i];
        if (arg[0] == '-')
        {
            switch (arg[1])
            {
            case 'a':                   // Attribute (Parameter)
                INC_ARG;
                INC_ARG;
                faun_setParameter(atoi(arg+2), 1, param(argv[i-1]),
                                  atof(argv[i]));
                break;

            case 'b':                   // Load Buffer
                INC_ARG;
                ch = atoi(arg+2);
                if (! faun_loadBuffer(ch, argv[i], offset, size))
                {
                    fprintf(stderr, "Command -b%d failed\n", ch);
                    faun_shutdown();
                    return EX_NOINPUT;
                }
                offset = size = 0;
                break;

            case 'c':                   // Control
                INC_ARG;
                faun_control(atoi(arg+2), 1, command(argv[i]));
                break;

            case 'f':                   // File Chunk
                INC_ARG;
                offset = atol(argv[i]);
                INC_ARG;
                size = atol(argv[i]);
                break;

            case 'm':                   // Play Music (Stream)
                si = atoi(arg+2);
                INC_ARG;
                INC_ARG;
                faun_playStream(si, argv[i], offset, size, hex(argv[i-1]));
                offset = size = 0;
                break;

            case 'o':                   // Begin program opcodes
                opcodeMode = 1;
                pc = program;
                break;

            case 'p':                   // Play Source
                INC_ARG;
                INC_ARG;
                faun_playSource(atoi(arg+2), hex(argv[i-1]), hex(argv[i]));
                break;

            case 's':                   // Stream Segment
            {
                double off, dur;
                INC_ARG;
                INC_ARG;
                off = atof(argv[i]);
                INC_ARG;
                dur = atof(argv[i]);
                faun_playStreamPart(si, off, dur, hex(argv[i-2]));
            }
                break;

            case 'W':                   // Wait for signal
                faun_closeOnSignal();
                faun_waitSignal(&sig);
                break;

            case 'z':                   // Suspend toggle
                faun_suspend(enabled);
                enabled ^= 1;
                break;

            default:
                goto invalid;
            }
        }
        else if (opcodeMode)
        {
            switch (arg[0])
            {
                case 'c':               // ca - Capture wave
                    *pc++ = FO_CAPTURE;
                    break;

                case 'e':               // en - End program
                                        // ep - Set source end position
                    if (arg[1] == 'p')
                    {
                        PUSH_OP_ARG(FO_SET_END);
                    }
                    else
                    {
                        *pc++ = FO_END;
                        opcodeMode = 0;
                        faun_program(0, program, pc - program);
                    }
                    break;

                case 'f':
                    switch (arg[1])
                    {
                        case 'i':       // fi - Fade In
                            *pc++ = FO_FADE_IN;
                            break;
                        default:
                        case 'o':       // fo - Fade Out
                            *pc++ = FO_FADE_OUT;
                            break;
                        case 'p':       // fp - Fade Period
                            PUSH_OP_ARG(FO_SET_FADE);
                            break;
                        /*
                        case 'y':       // fy - Fade Yes
                            *pc++ = FO_FADE_ON;
                            break;
                        case 'n':       // fn - Fade No
                            *pc++ = FO_FADE_OFF;
                            break;
                        */
                    }
                    break;

                case 'l':               // ly - Loop Yes
                                        // ln - Loop No
                    *pc++ = (arg[1] == 'n') ? FO_LOOP_OFF : FO_LOOP_ON;
                    break;

                case 'p':               // pb - Play buffer & set mode
                                        // pa - Pan
                    if (arg[1] == 'a') {
                        PUSH_OP_ARG(FO_PAN);
                        INC_ARG;
                        *pc++ = atoi(argv[i]);
                    } else {
                        PUSH_OP_ARG(FO_PLAY_BUF);
                        INC_ARG;
                        *pc++ = hex(argv[i]);
                    }
                    break;

                case 'q':               // qu - Queue buffer
                    PUSH_OP_ARG(FO_QUEUE);
                    break;

                case 's':               // so - Set Source
                                        // ss - Start Stream
                    if (arg[1] == 'o') {
                        PUSH_OP_ARG(FO_SOURCE);
                    } else {
                        *pc++ = FO_START_STREAM;
                        *pc++ = hex(arg + 2);
                    }
                    break;

                case 'v':               // vo - Volume Parameter
                                        // vc - Volume LR channels
                    if (arg[1] == 'o') {
                        PUSH_OP_ARG(FO_SET_VOL);
                    } else {
                        PUSH_OP_ARG(FO_VOL_LR);
                        INC_ARG;
                        *pc++ = atoi(argv[i]);
                    }
                    break;

                case 'w':               // wa - Wait 1/10 sec.
                    PUSH_OP_ARG(FO_WAIT);
                    break;

                default:
                    printf("Invalid program opcode %s\n", arg);
                    break;
            }
        }
        else if (arg[0] == '/')
        {
            ch = arg[1];
            if (ch >= '0' && ch <= '9')
                sleep(ch - '0');
        }
        else
        {
invalid:
            printf("Invalid option %s\n", arg);
            break;
        }
    }

    faun_shutdown();
    return 0;
}
