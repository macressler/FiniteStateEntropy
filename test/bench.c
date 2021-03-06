/*
    bench.c - Demo module to benchmark open-source compression algorithm
    Copyright (C) Yann Collet 2012-2015

    GPL v2 License

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

    You can contact the author at :
    - FSE source repository : https://github.com/Cyan4973/FiniteStateEntropy
    - Public forum : https://groups.google.com/forum/#!forum/lz4c
*/


/**************************************
*  Tuning parameters
***************************************/
#define NBLOOPS    4
#define TIMELOOP   2500


/**************************************
*  Compiler Options
***************************************/
/* Disable some Visual warning messages */
#define _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_DEPRECATE     /* VS2005 */

/* Unix Large Files support (>4GB) */
#define _FILE_OFFSET_BITS 64
#if (defined(__sun__) && (!defined(__LP64__)))   /* Sun Solaris 32-bits requires specific definitions */
#  define _LARGEFILE_SOURCE
#elif ! defined(__LP64__)                        /* No point defining Large file for 64 bit */
#  define _LARGEFILE64_SOURCE
#endif

/* S_ISREG & gettimeofday() are not supported by MSVC */
#if defined(_MSC_VER) || defined(_WIN32)
#  define BMK_LEGACY_TIMER 1
#endif


/**************************************
*  Includes
***************************************/
#include <stdlib.h>      /* malloc */
#include <stdio.h>       /* fprintf, fopen, ftello64 */
#include <string.h>      // strcat
#include <sys/types.h>   // stat64
#include <sys/stat.h>    // stat64

// Use ftime() if gettimeofday() is not available on your target
#if defined(BMK_LEGACY_TIMER)
#  include <sys/timeb.h> /* timeb, ftime */
#else
#  include <sys/time.h>  /* gettimeofday */
#endif

#include "bench.h"
#include "fileio.h"
#include "fse.h"
#include "fseU16.h"
#include "zlibh.h"
#include "xxhash.h"


/**************************************
*  Compiler specifics
***************************************/
#if !defined(S_ISREG)
#  define S_ISREG(x) (((x) & S_IFMT) == S_IFREG)
#endif

#if defined(_MSC_VER)   /* Visual */
#  pragma warning(disable : 4127)        /*disable: C4127: conditional expression is constant */
#  include <intrin.h>
#endif


/**************************************
*  Basic Types
***************************************/
#if defined (__STDC_VERSION__) && __STDC_VERSION__ >= 199901L   /* C99 */
# include <stdint.h>
typedef uint8_t  BYTE;
typedef uint16_t U16;
typedef  int16_t S16;
typedef uint32_t U32;
typedef  int32_t S32;
typedef uint64_t U64;
#else
typedef unsigned char       BYTE;
typedef unsigned short      U16;
typedef   signed short      S16;
typedef unsigned int        U32;
typedef   signed int        S32;
typedef unsigned long long  U64;
#endif


/**************************************
*  Constants
***************************************/
#define KB *(1U<<10)
#define MB *(1U<<20)
#define GB *(1U<<30)

#define KNUTH               2654435761U
#define MAX_MEM             ((sizeof(void*)==4) ? (2 GB - 64 MB) : (9ULL GB))
#define DEFAULT_CHUNKSIZE   (32 KB)


/**************************************
*  MACRO
***************************************/
#define DISPLAY(...) fprintf(stderr, __VA_ARGS__)


/**************************************
*  Benchmark Parameters
***************************************/
static U32 chunkSize = DEFAULT_CHUNKSIZE;
static int nbIterations = NBLOOPS;
static int BMK_byteCompressor = 1;
static int BMK_tableLog = 0;

void BMK_SetByteCompressor(int id) { BMK_byteCompressor = id; }

void BMK_SetBlocksize(U32 bsize) { chunkSize = bsize; }

void BMK_SetTableLog(int tableLog) { BMK_tableLog = 5 + tableLog; }

void BMK_SetNbIterations(int nbLoops)
{
    nbIterations = nbLoops;
    DISPLAY("- %i iterations -\n", nbIterations);
}

typedef struct
{
    unsigned id;
    char*  origBuffer;
    size_t origSize;
    char*  compressedBuffer;
    size_t compressedSize;
    char*  destBuffer;
    size_t destSize;
} chunkParameters_t;


/*********************************************************
*  local functions
*********************************************************/

#if defined(BMK_LEGACY_TIMER)

static int BMK_GetMilliStart(void)
{
    /* Based on Legacy ftime()
    *  Rolls over every ~ 12.1 days (0x100000/24/60/60)
    *  Use GetMilliSpan to correct for rollover */
    struct timeb tb;
    int nCount;
    ftime( &tb );
    nCount = (int) (tb.millitm + (tb.time & 0xfffff) * 1000);
    return nCount;
}

#else

static int BMK_GetMilliStart(void)
{
    /* Based on newer gettimeofday()
    *  Use GetMilliSpan to correct for rollover */
    struct timeval tv;
    int nCount;
    gettimeofday(&tv, NULL);
    nCount = (int) (tv.tv_usec/1000 + (tv.tv_sec & 0xfffff) * 1000);
    return nCount;
}

#endif


static int BMK_GetMilliSpan( int nTimeStart )
{
    int nSpan = BMK_GetMilliStart() - nTimeStart;
    if ( nSpan < 0 )
        nSpan += 0x100000 * 1000;
    return nSpan;
}


static size_t BMK_findMaxMem(U64 requiredMem)
{
    size_t step = (64 MB);
    BYTE* testmem=NULL;

    requiredMem = (((requiredMem >> 26) + 1) << 26);
    requiredMem += 2*step;
    if (requiredMem > MAX_MEM) requiredMem = MAX_MEM;

    while (!testmem)
    {
        requiredMem -= step;
        if (requiredMem <= step)
        {
            requiredMem = step+64;
            break;
        }
        testmem = (BYTE*) malloc ((size_t)requiredMem);
    }

    free (testmem);
    return (size_t) (requiredMem - step);
}


static U64 BMK_GetFileSize(char* infilename)
{
    int r;
#if defined(_MSC_VER)
    struct _stat64 statbuf;
    r = _stat64(infilename, &statbuf);
#else
    struct stat statbuf;
    r = stat(infilename, &statbuf);
#endif
    if (r || !S_ISREG(statbuf.st_mode)) return 0;   /* No good... */
    return (U64)statbuf.st_size;
}


/*********************************************************
*  Public function
*********************************************************/

void BMK_benchMem285(chunkParameters_t* chunkP, int nbChunks, char* inFileName, int benchedSize,
                  U64* totalCompressedSize, double* totalCompressionTime, double* totalDecompressionTime,
                  int memLog)
{
    int loopNb, chunkNb;
    size_t cSize=0;
    double fastestC = 100000000., fastestD = 100000000.;
    double ratio=0.;
    U32 crcCheck=0;
    U32 crcOrig;

    /* Init */
    crcOrig = XXH32(chunkP[0].origBuffer, benchedSize,0);

    DISPLAY("\r%79s\r", "");
    for (loopNb = 1; loopNb <= nbIterations; loopNb++)
    {
        int nbLoops;
        int milliTime;

        /* Compression benchmark */
        DISPLAY("%1i-%-14.14s : %9i ->\r", loopNb, inFileName, benchedSize);
        { int i; for (i=0; i<benchedSize; i++) chunkP[0].compressedBuffer[i]=(char)i; }     /* warmimg up memory */

        nbLoops = 0;
        milliTime = BMK_GetMilliStart();
        while(BMK_GetMilliStart() == milliTime);
        milliTime = BMK_GetMilliStart();
        while(BMK_GetMilliSpan(milliTime) < TIMELOOP)
        {
            for (chunkNb=0; chunkNb<nbChunks; chunkNb++)
            {
                const void* rawPtr = chunkP[chunkNb].origBuffer;
                const U16* U16chunkPtr = (const U16*) rawPtr;
                chunkP[chunkNb].compressedSize = FSE_compressU16(chunkP[chunkNb].compressedBuffer, chunkP[chunkNb].origSize, U16chunkPtr, chunkP[chunkNb].origSize/2, 0, memLog);
            }
            nbLoops++;
        }
        milliTime = BMK_GetMilliSpan(milliTime);

        if ((double)milliTime < fastestC*nbLoops) fastestC = (double)milliTime/nbLoops;
        cSize=0; for (chunkNb=0; chunkNb<nbChunks; chunkNb++) cSize += chunkP[chunkNb].compressedSize;
        ratio = (double)cSize/(double)benchedSize*100.;

        DISPLAY("%1i-%-14.14s : %9i -> %9i (%5.2f%%),%7.1f MB/s\r", loopNb, inFileName, (int)benchedSize, (int)cSize, ratio, (double)benchedSize / fastestC / 1000.);

        //DISPLAY("\n"); continue;   // skip decompression
        // Decompression
        //{ size_t i; for (i=0; i<benchedSize; i++) orig_buff[i]=0; }     // zeroing area, for CRC checking

        nbLoops = 0;
        milliTime = BMK_GetMilliStart();
        while(BMK_GetMilliStart() == milliTime);
        milliTime = BMK_GetMilliStart();
        while(BMK_GetMilliSpan(milliTime) < TIMELOOP)
        {
            for (chunkNb=0; chunkNb<nbChunks; chunkNb++)
            {
                void* rawPtr = chunkP[chunkNb].destBuffer;
                U16* U16dstPtr = (U16*)rawPtr;
                chunkP[chunkNb].compressedSize = FSE_decompressU16(U16dstPtr, chunkP[chunkNb].origSize/2, chunkP[chunkNb].compressedBuffer, chunkP[chunkNb].compressedSize);
            }
            nbLoops++;
        }
        milliTime = BMK_GetMilliSpan(milliTime);

        if ((double)milliTime < fastestD*nbLoops) fastestD = (double)milliTime/nbLoops;
        DISPLAY("%1i-%-14.14s : %9i -> %9i (%5.2f%%),%7.1f MB/s ,%7.1f MB/s\r", loopNb, inFileName, (int)benchedSize, (int)cSize, ratio, (double)benchedSize / fastestC / 1000., (double)benchedSize / fastestD / 1000.);

        /* CRC Checking */
        crcCheck = XXH32(chunkP[0].destBuffer, benchedSize,0);
        if (crcOrig!=crcCheck)
        {
            const char* src = chunkP[0].origBuffer;
            const char* fin = chunkP[0].destBuffer;
            const char* const srcStart = src;
            while (*src==*fin) src++, fin++;
            DISPLAY("\n!!! %14s : Invalid Checksum !!! pos %i/%i\n", inFileName, (int)(src-srcStart), benchedSize);
            break;
        }
    }

    if (crcOrig==crcCheck)
    {
        if (ratio<100.)
            DISPLAY("%-16.16s : %9i -> %9i (%5.2f%%),%7.1f MB/s ,%7.1f MB/s\n", inFileName, (int)benchedSize, (int)cSize, ratio, (double)benchedSize / fastestC / 1000., (double)benchedSize / fastestD / 1000.);
        else
            DISPLAY("%-16.16s : %9i -> %9i (%5.1f%%),%7.1f MB/s ,%7.1f MB/s \n", inFileName, (int)benchedSize, (int)cSize, ratio, (double)benchedSize / fastestC / 1000., (double)benchedSize / fastestD / 1000.);
    }
    *totalCompressedSize    += cSize;
    *totalCompressionTime   += fastestC;
    *totalDecompressionTime += fastestD;
}


size_t BMK_ZLIBH_compress(void* dst, size_t dstSize, const void* src, size_t srcSize, unsigned nbSymbols, unsigned tableLog)
{ (void)nbSymbols; (void)tableLog; (void)dstSize; return ZLIBH_compress((char*)dst, (const char*)src, (int)srcSize); }

size_t BMK_ZLIBH_decompress(void* dest, size_t originalSize, const void* compressed, size_t cSize)
{ (void)cSize; ZLIBH_decompress((char*)dest, (const char*)compressed); return originalSize; }


void BMK_benchMem(chunkParameters_t* chunkP, int nbChunks, char* inFileName, int benchedSize,
                  U64* totalCompressedSize, double* totalCompressionTime, double* totalDecompressionTime,
                  int nbSymbols, int memLog)
{
    int loopNb, chunkNb;
    size_t cSize=0;
    double fastestC = 100000000., fastestD = 100000000.;
    double ratio=0.;
    U32 crcCheck=0;
    U32 crcOrig;
    size_t (*compressor)(void* dst, size_t, const void* src, size_t, unsigned, unsigned);
    size_t (*decompressor)(void* dst, size_t maxDstSize, const void* cSrc, size_t cSrcSize);

    /* Init */
    if (nbSymbols==3) { BMK_benchMem285 (chunkP, nbChunks, inFileName, benchedSize, totalCompressedSize, totalCompressionTime, totalDecompressionTime, memLog); return; }
    crcOrig = XXH32(chunkP[0].origBuffer, benchedSize,0);
    switch(BMK_byteCompressor)
    {
    case 3:
        compressor = BMK_ZLIBH_compress;
        decompressor = BMK_ZLIBH_decompress;
        break;
    default:
        compressor = FSE_compress2;
        decompressor = FSE_decompress;
    }

    DISPLAY("\r%79s\r", "");
    for (loopNb = 1; loopNb <= nbIterations; loopNb++)
    {
        int nbLoops;
        int milliTime;

        /* Compression */
        DISPLAY("%1i-%-14.14s : %9i ->\r", loopNb, inFileName, benchedSize);
        { int i; for (i=0; i<benchedSize; i++) chunkP[0].compressedBuffer[i]=(char)i; }     /* warmimg up memory */

        nbLoops = 0;
        milliTime = BMK_GetMilliStart();
        while(BMK_GetMilliStart() == milliTime);
        milliTime = BMK_GetMilliStart();
        while(BMK_GetMilliSpan(milliTime) < TIMELOOP)
        {
            for (chunkNb=0; chunkNb<nbChunks; chunkNb++)
            {
                size_t cBSize = compressor(chunkP[chunkNb].compressedBuffer, FSE_compressBound(chunkP[chunkNb].origSize), chunkP[chunkNb].origBuffer, chunkP[chunkNb].origSize, nbSymbols, memLog);
                if (FSE_isError(cBSize)) { DISPLAY("!!! Error compressing block %i  !!!!    \n", chunkNb); return; }
                chunkP[chunkNb].compressedSize = cBSize;
            }
            nbLoops++;
        }
        milliTime = BMK_GetMilliSpan(milliTime);

        if ((double)milliTime < fastestC*nbLoops) fastestC = (double)milliTime/nbLoops;
        cSize=0; for (chunkNb=0; chunkNb<nbChunks; chunkNb++) cSize += chunkP[chunkNb].compressedSize;
        ratio = (double)cSize/(double)benchedSize*100.;

        DISPLAY("%1i-%-14.14s : %9i -> %9i (%5.2f%%),%7.1f MB/s\r", loopNb, inFileName, (int)benchedSize, (int)cSize, ratio, (double)benchedSize / fastestC / 1000.);

        /* if (loopNb == nbIterations) DISPLAY("\n"); continue;   *//* skip decompression */
        /* Decompression */
        { int i; for (i=0; i<benchedSize; i++) chunkP[0].destBuffer[i]=0; }     /* zeroing area, for CRC checking */

        nbLoops = 0;
        milliTime = BMK_GetMilliStart();
        while(BMK_GetMilliStart() == milliTime);
        milliTime = BMK_GetMilliStart();
        while(BMK_GetMilliSpan(milliTime) < TIMELOOP)
        {
            for (chunkNb=0; chunkNb<nbChunks; chunkNb++)
            {
                size_t regenSize;
                switch(chunkP[chunkNb].compressedSize)
                {
                case 0:
                    regenSize = chunkP[chunkNb].origSize;
                    memcpy(chunkP[chunkNb].destBuffer, chunkP[chunkNb].origBuffer, regenSize);
                    break;
                case 1:
                    regenSize = chunkP[chunkNb].origSize;
                    memset(chunkP[chunkNb].destBuffer, chunkP[chunkNb].origBuffer[0], chunkP[chunkNb].origSize);
                    break;
                default:
                    regenSize = decompressor(chunkP[chunkNb].destBuffer, chunkP[chunkNb].origSize, chunkP[chunkNb].compressedBuffer, chunkP[chunkNb].compressedSize);
                }

                if (regenSize != chunkP[chunkNb].origSize)
                {
                    DISPLAY("!!! Error decompressing block %i !!!! => (%s)   \n", chunkNb, FSE_getErrorName(regenSize));
                    return;
                }
            }
            nbLoops++;
        }
        milliTime = BMK_GetMilliSpan(milliTime);

        if ((double)milliTime < fastestD*nbLoops) fastestD = (double)milliTime/nbLoops;
        DISPLAY("%1i-%-14.14s : %9i -> %9i (%5.2f%%),%7.1f MB/s ,%7.1f MB/s\r", loopNb, inFileName, (int)benchedSize, (int)cSize, ratio, (double)benchedSize / fastestC / 1000., (double)benchedSize / fastestD / 1000.);

        /* CRC Checking */
        crcCheck = XXH32(chunkP[0].destBuffer, benchedSize,0);
        if (crcOrig!=crcCheck)
        {
            const char* src = chunkP[0].origBuffer;
            const char* fin = chunkP[0].destBuffer;
            const char* const srcStart = src;
            while (*src==*fin)
                src++, fin++;
            DISPLAY("\n!!! %14s : Invalid Checksum !!! pos %i/%i\n", inFileName, (int)(src-srcStart), benchedSize);
            break;
        }

    }

    if (crcOrig==crcCheck)
    {
        if (ratio<100.)
            DISPLAY("%-16.16s : %9i -> %9i (%5.2f%%),%7.1f MB/s ,%7.1f MB/s\n", inFileName, (int)benchedSize, (int)cSize, ratio, (double)benchedSize / fastestC / 1000., (double)benchedSize / fastestD / 1000.);
        else
            DISPLAY("%-16.16s : %9i -> %9i (%5.1f%%),%7.1f MB/s ,%7.1f MB/s \n", inFileName, (int)benchedSize, (int)cSize, ratio, (double)benchedSize / fastestC / 1000., (double)benchedSize / fastestD / 1000.);
    }
    *totalCompressedSize    += cSize;
    *totalCompressionTime   += fastestC;
    *totalDecompressionTime += fastestD;
}


int BMK_benchFiles(char** fileNamesTable, int nbFiles)
{
    int fileIdx=0;
    char* orig_buff;
    U64 totals = 0;
    U64 totalz = 0;
    double totalc = 0.;
    double totald = 0.;


    while (fileIdx<nbFiles)
    {
        FILE*  inFile;
        char*  inFileName;
        U64    inFileSize;
        size_t benchedSize;
        int nbChunks;
        int maxCompressedChunkSize;
        size_t readSize;
        char* compressedBuffer; int compressedBuffSize;
        char* destBuffer;
        chunkParameters_t* chunkP;

        /* Check file existence */
        inFileName = fileNamesTable[fileIdx++];
        inFile = fopen( inFileName, "rb" );
        if (inFile==NULL) { DISPLAY( "Pb opening %s\n", inFileName); return 11; }

        /* Memory size evaluation */
        inFileSize = BMK_GetFileSize(inFileName);
        if (inFileSize==0) { DISPLAY( "file is empty\n"); fclose(inFile); return 11; }
        benchedSize = (size_t) BMK_findMaxMem(inFileSize * 3) / 3;
        if ((U64)benchedSize > inFileSize) benchedSize = (size_t)inFileSize;
        if (benchedSize < inFileSize) DISPLAY("Not enough memory for '%s' full size; testing %i MB only...\n", inFileName, (int)(benchedSize>>20));

        /* Allocation */
        chunkP = (chunkParameters_t*) malloc(((benchedSize / chunkSize)+1) * sizeof(chunkParameters_t));
        orig_buff = (char*)malloc((size_t )benchedSize);
        nbChunks = (int) (benchedSize / chunkSize) + 1;
        maxCompressedChunkSize = (int)FSE_compressBound(chunkSize);
        compressedBuffSize = nbChunks * maxCompressedChunkSize;
        compressedBuffer = (char*)malloc((size_t )compressedBuffSize);
        destBuffer = (char*)malloc((size_t )benchedSize);


        if (!orig_buff || !compressedBuffer || !destBuffer || !chunkP)
        {
            DISPLAY("\nError: not enough memory!\n");
            free(orig_buff);
            free(compressedBuffer);
            free(destBuffer);
            free(chunkP);
            fclose(inFile);
            return 12;
        }

        /* Init chunks */
        {
            int i;
            size_t remaining = benchedSize;
            char* in = orig_buff;
            char* out = compressedBuffer;
            char* dst = destBuffer;
            for (i=0; i<nbChunks; i++)
            {
                chunkP[i].id = i;
                chunkP[i].origBuffer = in; in += chunkSize;
                if (remaining > chunkSize) { chunkP[i].origSize = chunkSize; remaining -= chunkSize; } else { chunkP[i].origSize = (int)remaining; remaining = 0; }
                chunkP[i].compressedBuffer = out; out += maxCompressedChunkSize;
                chunkP[i].compressedSize = 0;
                chunkP[i].destBuffer = dst; dst += chunkSize;
            }
        }

        /* Fill input buffer */
        DISPLAY("Loading %s...       \r", inFileName);
        readSize = fread(orig_buff, 1, benchedSize, inFile);
        fclose(inFile);

        if (readSize != benchedSize)
        {
            DISPLAY("\nError: problem reading file '%s' (%i read, should be %i) !!    \n", inFileName, (int)readSize, (int)benchedSize);
            free(orig_buff);
            free(compressedBuffer);
            free(destBuffer);
            free(chunkP);
            return 13;
        }

        /* Bench */
        BMK_benchMem(chunkP, nbChunks, inFileName, (int)benchedSize, &totalz, &totalc, &totald, 255, BMK_tableLog);
        totals += benchedSize;

        free(orig_buff);
        free(compressedBuffer);
        free(destBuffer);
        free(chunkP);
    }

    if (nbFiles > 1)
        DISPLAY("%-16.16s :%10llu ->%10llu (%5.2f%%), %6.1f MB/s , %6.1f MB/s\n", "  TOTAL", (long long unsigned int)totals, (long long unsigned int)totalz, (double)totalz/(double)totals*100., (double)totals/totalc/1000., (double)totals/totald/1000.);

    return 0;
}



/**********************************************************************
*  BenchCore
**********************************************************************/

static void BMK_benchCore_Mem(char* dst,
                              char* src, unsigned benchedSize,
                              unsigned nbSymbols, unsigned tableLog, char* inFileName,
                              U64* totalCompressedSize, double* totalCompressionTime, double* totalDecompressionTime)
{
    int loopNb;
    size_t cSize=0, dSize=0;
    double fastestC = 100000000., fastestD = 100000000.;
    double ratio=0.;
    U64 crcCheck=0;
    U64 crcOrig;
    U32 count[256];
    short norm[256];
    FSE_CTable* ct;
    FSE_DTable* dt;
    size_t fastMode;

    /* Init */
    crcOrig = XXH64(src, benchedSize,0);
    FSE_count(count, &nbSymbols, (BYTE*)src, benchedSize);
    tableLog = (U32)FSE_normalizeCount(norm, tableLog, count, benchedSize, nbSymbols);
    ct = FSE_createCTable(tableLog, nbSymbols);
    FSE_buildCTable(ct, norm, nbSymbols, tableLog);
    dt = FSE_createDTable(tableLog);
    fastMode = FSE_buildDTable(dt, norm, nbSymbols, tableLog);

    DISPLAY("\r%79s\r", "");
    for (loopNb = 1; loopNb <= nbIterations; loopNb++)
    {
        int nbLoops;
        int milliTime;

        /* Compression */
        DISPLAY("%1i-%-14.14s : %9u ->\r", loopNb, inFileName, benchedSize);
        { unsigned i; for (i=0; i<benchedSize; i++) dst[i]=(char)i; }     /* warmimg up memory */

        nbLoops = 0;
        milliTime = BMK_GetMilliStart();
        while(BMK_GetMilliStart() == milliTime);
        milliTime = BMK_GetMilliStart();
        while(BMK_GetMilliSpan(milliTime) < TIMELOOP)
        {
            cSize = FSE_compress_usingCTable(dst, FSE_compressBound(benchedSize), src, benchedSize, ct);
            nbLoops++;
        }
        milliTime = BMK_GetMilliSpan(milliTime);

        if (FSE_isError(cSize)) { DISPLAY("!!! Error compressing file %s !!!!    \n", inFileName); break; }

        if ((double)milliTime < fastestC*nbLoops) fastestC = (double)milliTime/nbLoops;
        ratio = (double)cSize/(double)benchedSize*100.;

        DISPLAY("%1i-%-14.14s : %9i -> %9i (%5.2f%%),%7.1f MB/s\r", loopNb, inFileName, (int)benchedSize, (int)cSize, ratio, (double)benchedSize / fastestC / 1000.);

        /* Decompression */
        { unsigned i; for (i=0; i<benchedSize; i++) src[i]=0; }     /* zeroing area, for CRC checking */

        nbLoops = 0;
        milliTime = BMK_GetMilliStart();
        while(BMK_GetMilliStart() == milliTime);
        milliTime = BMK_GetMilliStart();
        while(BMK_GetMilliSpan(milliTime) < TIMELOOP)
        {
            dSize = FSE_decompress_usingDTable(src, benchedSize, dst, cSize, dt, fastMode);
            nbLoops++;
        }
        milliTime = BMK_GetMilliSpan(milliTime);

        if (FSE_isError(dSize)) { DISPLAY("\n!!! Error decompressing file %s !!!!    \n", inFileName); break; }
        if (dSize != benchedSize) { DISPLAY("\n!!! Error decompressing file %s !!!!    \n", inFileName); break; }

        if ((double)milliTime < fastestD*nbLoops) fastestD = (double)milliTime/nbLoops;
        DISPLAY("%1i-%-14.14s : %9i -> %9i (%5.2f%%),%7.1f MB/s ,%7.1f MB/s\r", loopNb, inFileName, (int)benchedSize, (int)cSize, ratio, (double)benchedSize / fastestC / 1000., (double)benchedSize / fastestD / 1000.);

        /* CRC Checking */
        crcCheck = XXH64(src, benchedSize, 0);
        if (crcOrig!=crcCheck) { DISPLAY("\n!!! WARNING !!! %14s : Invalid Checksum : %x != %x\n", inFileName, (unsigned)crcOrig, (unsigned)crcCheck); break; }
    }

    if (crcOrig==crcCheck)
    {
        if (ratio<100.)
            DISPLAY("%-16.16s : %9i -> %9i (%5.2f%%),%7.1f MB/s ,%7.1f MB/s\n", inFileName, (int)benchedSize, (int)cSize, ratio, (double)benchedSize / fastestC / 1000., (double)benchedSize / fastestD / 1000.);
        else
            DISPLAY("%-16.16s : %9i -> %9i (%5.1f%%),%7.1f MB/s ,%7.1f MB/s \n", inFileName, (int)benchedSize, (int)cSize, ratio, (double)benchedSize / fastestC / 1000., (double)benchedSize / fastestD / 1000.);
    }
    *totalCompressedSize    += cSize;
    *totalCompressionTime   += fastestC;
    *totalDecompressionTime += fastestD;

    free(ct);
    free(dt);
}


int BMK_benchCore_Files(char** fileNamesTable, int nbFiles)
{
    int fileIdx=0;

    U64 totals = 0;
    U64 totalz = 0;
    double totalc = 0.;
    double totald = 0.;

    // Default
    if (BMK_tableLog==0) BMK_tableLog=12;

    // Loop for each file
    while (fileIdx<nbFiles)
    {
        FILE*  inFile;
        char*  inFileName;
        U64    inFileSize;
        size_t benchedSize;
        int nbChunks;
        size_t maxCompressedChunkSize;
        size_t readSize;
        char* orig_buff;
        char* compressedBuffer; size_t compressedBuffSize;

        /* Check file existence */
        inFileName = fileNamesTable[fileIdx++];
        inFile = fopen( inFileName, "rb" );
        if (inFile==NULL) { DISPLAY( "Pb opening %s\n", inFileName); return 11; }

        /* Memory allocation & restrictions */
        inFileSize = BMK_GetFileSize(inFileName);
        if (inFileSize==0) { DISPLAY( "%s is empty\n", inFileName); return 11; }
        benchedSize = 16 MB;
        if ((U64)benchedSize > inFileSize) benchedSize = (size_t)inFileSize;
        else DISPLAY("FSE Core Loop speed evaluation, testing %i KB ...\n", (int)(benchedSize>>10));

        /* Alloc */
        orig_buff = (char*)malloc(benchedSize);
        nbChunks = 1;
        maxCompressedChunkSize = FSE_compressBound((int)benchedSize);
        compressedBuffSize = nbChunks * maxCompressedChunkSize;
        compressedBuffer = (char*)malloc(compressedBuffSize);

        if (!orig_buff || !compressedBuffer)
        {
            DISPLAY("\nError: not enough memory!\n");
            free(orig_buff);
            free(compressedBuffer);
            fclose(inFile);
            return 12;
        }

        /* Fill input buffer */
        DISPLAY("Loading %s...       \r", inFileName);
        readSize = fread(orig_buff, 1, benchedSize, inFile);
        fclose(inFile);

        if (readSize != benchedSize)
        {
            DISPLAY("\nError: problem reading file '%s' (%i read, should be %i) !!    \n", inFileName, (int)readSize, (int)benchedSize);
            free(orig_buff);
            free(compressedBuffer);
            return 13;
        }

        /* Bench */
        BMK_benchCore_Mem(compressedBuffer, orig_buff, (int)benchedSize, 255, BMK_tableLog, inFileName, &totalz, &totalc, &totald);
        totals += benchedSize;

        free(orig_buff);
        free(compressedBuffer);
    }

    if (nbFiles > 1)
        DISPLAY("%-16.16s :%10llu ->%10llu (%5.2f%%), %6.1f MB/s , %6.1f MB/s\n", "  TOTAL", (long long unsigned int)totals, (long long unsigned int)totalz, (double)totalz/(double)totals*100., (double)totals/totalc/1000., (double)totals/totald/1000.);

    return 0;
}
