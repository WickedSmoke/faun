/*
    Function to read WAVE headers.

    Written in 2017-2018 by Karl Robillard <wickedsmoke@users.sf.net>

    To the extent possible under law, the author(s) have dedicated all
    copyright and related and neighboring rights to this software to the public
    domain worldwide. This software is distributed without any warranty.

    You should have received a copy of the CC0 Public Domain Dedication along
    with this software. If not, see
    <http://creativecommons.org/publicdomain/zero/1.0/>.
*/


#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define MAKE_ID(a,b,c,d)    ((a<<24)|(b<<16)|(c<<8)|d)
#define SWAP32(pn)          reverse4((char*) &(pn))
#define SWAP16(pn)          reverse2((char*) &(pn))
void reverse4( char* cp )
{
    int tmp = cp[0];
    cp[0] = cp[3];
    cp[3] = tmp;
    tmp = cp[1];
    cp[1] = cp[2];
    cp[2] = tmp;
}
void reverse2( char* cp )
{
    int tmp = cp[0];
    cp[0] = cp[1];
    cp[1] = tmp;
}
#else
#define MAKE_ID(a,b,c,d)    ((d<<24)|(c<<16)|(b<<8)|a)
#endif


#define ID_RIFF MAKE_ID('R','I','F','F')
#define ID_WAVE MAKE_ID('W','A','V','E')
#define ID_FMT  MAKE_ID('f','m','t',' ')
#define ID_FACT MAKE_ID('f','a','c','t')
#define ID_DATA MAKE_ID('d','a','t','a')


// See RFC 2361 WAVE and AVI Codec Registries for a full list of formats.
// (https://tools.ietf.org/html/rfc2361)
enum WavFormat
{
    WAV_PCM         = 1,
    WAV_IEEE_FLOAT  = 3,
    WAV_8BIT_A_LAW  = 6,
    WAV_8BIT_MU_LAW = 7,
    WAV_EXTENSIBLE  = 0xFFFE
};


typedef struct
{
    uint32_t idRIFF;
    uint32_t riffSize;          // File length - 8 bytes
    uint32_t idWAVE;
    uint32_t idFmt;
    uint32_t fmtSize;
    uint16_t format;
    uint16_t channels;
    uint32_t sampleRate;        // Sampling rate (blocks per second)
    uint32_t byteRate;          // sampleRate * channels * bitsPerSample/8
    uint16_t blockAlign;        // channels * bitsPerSample/8
    uint16_t bitsPerSample;     // 8, 16, 24, etc.

    uint16_t cbSize;            // Size of extension
    uint16_t validBitsPerSample;
    uint32_t channelMask;
    uint16_t subFormat;         // A 16 byte GUID; first 2 bytes are format.

    uint32_t idData;            // "data" or "FLLR" string
    uint32_t dataSize;          // numSamples * channels * bitsPerSample/8
}
WavHeader;


// Header points to the ID and size words.
static int wav_skipChunk( FILE* fp, uint32_t* header )
{
    size_t n;
    if( fseek( fp, header[1], SEEK_CUR ) )
        return 0;
    n = fread( header, sizeof(uint32_t), 2, fp );
    if( n != 2 )
        return 0;
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    SWAP32( header[1] );
#endif
    return 1;
}


#define WAV_ERROR_READ  1
#define WAV_ERROR_ID    2
#define WAV_ERROR_CHUNK 3

/*
 * Read WAVE header from file.
 *
 * Return zero on success or one of the following error codes:
 *    1 - Read error
 *    2 - Not RIFF/WAVE
 *    3 - Missing FMT or DATA chunk
 */
int wav_readHeader( FILE* fp, WavHeader* wh )
{
    size_t n;
    const size_t sizeId5 = 5 * sizeof(uint32_t);
    const size_t stdFmtSize = 16;
    const size_t extSize = 10;
    const size_t extFmtSize = stdFmtSize+extSize;

    // Read RIFF header and first chunk header.
    n = fread( wh, 1, sizeId5, fp );
    if( n != sizeId5 )
        return WAV_ERROR_READ;
    if( wh->idRIFF != ID_RIFF || wh->idWAVE != ID_WAVE )
        return WAV_ERROR_ID;
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    SWAP32( wh->fmtSize );
#endif

    // Search for format chunk.
    while( wh->idFmt != ID_FMT )
    {
        if( ! wav_skipChunk( fp, &wh->idFmt ) )
            return WAV_ERROR_CHUNK;
    }

    n = fread( &wh->format, 1, stdFmtSize, fp );
    if( n != stdFmtSize )
        return WAV_ERROR_READ;

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    SWAP32( wh->riffSize );
    SWAP32( wh->fmtSize );
    SWAP16( wh->format );
    SWAP16( wh->channels );
    SWAP32( wh->sampleRate );
    SWAP32( wh->buteRate );
    SWAP16( wh->blockAlign );
    SWAP16( wh->bitsPerSample );
#endif

    if( wh->fmtSize > stdFmtSize )
    {
        if( wh->format == WAV_EXTENSIBLE && wh->fmtSize >= extFmtSize )
        {
            n = fread( &wh->cbSize, 1, extSize, fp );
            if( n != extSize )
                return WAV_ERROR_READ;

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
            SWAP16( wh->cbSize );
            SWAP16( wh->validBitsPerSample );
            SWAP32( wh->channelMask );
            SWAP16( wh->subFormat );
#endif

            if( fseek( fp, wh->fmtSize - extFmtSize, SEEK_CUR ) )
                return WAV_ERROR_READ;
        }
        else
        {
            if( fseek( fp, wh->fmtSize - stdFmtSize, SEEK_CUR ) )
                return WAV_ERROR_READ;
        }
    }

    n = fread( &wh->idData, sizeof(uint32_t), 2, fp );
    if( n != 2 )
        return WAV_ERROR_READ;
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    SWAP32( wh->dataSize );
#endif

    while( wh->idData != ID_DATA )
    {
        if( ! wav_skipChunk( fp, &wh->idData ) )
            return WAV_ERROR_CHUNK;
    }
    return 0;
}


int wav_formatExt( WavHeader* wh )
{
    return (wh->format == WAV_EXTENSIBLE) ? wh->subFormat : wh->format;
}


/* Return number of samples per channel. */
int wav_sampleCount( const WavHeader* wh )
{
    int sampleBytes = wh->bitsPerSample / 8;
    if( ! sampleBytes || ! wh->channels )
        return 0;
    return wh->dataSize / wh->channels / sampleBytes;
}


#ifdef DEBUG
void wav_dumpHeader( FILE* fp, const WavHeader* wh, const char* prelude,
                     const char* indent )
{
#define DUMP(str)   fprintf( fp, "%s" #str ":%u\n",indent,wh->str)
    if( prelude )
        fprintf( fp, "%s\n", prelude );
    DUMP( riffSize );

    fprintf( fp, "%s---\n", indent );
    DUMP( fmtSize );
    DUMP( format );
    DUMP( channels );
    DUMP( sampleRate );
    DUMP( byteRate );
    DUMP( blockAlign );
    DUMP( bitsPerSample );

    if( wh->format == WAV_EXTENSIBLE )
    {
        fprintf( fp, "%s---\n", indent );
        DUMP( cbSize );
        DUMP( validBitsPerSample );
        DUMP( channelMask );
        DUMP( subFormat );
    }

    fprintf( fp, "%s---\n", indent );
    fprintf( fp, "%sdataSize:%u (%d samples)\n", indent, wh->dataSize,
             wav_sampleCount( wh ) );
}
#endif

#ifdef UNIT_TEST
// cc wav_read.c -DDEBUG -DUNIT_TEST
int main(int argc, char** argv)
{
    WavHeader wh;
    FILE* fp = fopen(argv[1], "rb");
    if (! fp) {
        fprintf(stderr, "Cannot open file %s\n", argv[1]);
        return 66;  // EX_NOINPUT
    }
    int err = wav_readHeader(fp, &wh);
    if (err) {
        fprintf(stderr, "wav_readHeader error %d\n", err);
        return 1;
    }
    wav_dumpHeader(stdout, &wh, "WavHeader", "");
    fclose(fp);
    return 0;
}
#endif
