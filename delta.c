/*
** Copyright (c) 2006 D. Richard Hipp
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of the Simplified BSD License (also
** known as the "2-Clause License" or "FreeBSD License".)

** This program is distributed in the hope that it will be useful,
** but without any warranty; without even the implied warranty of
** merchantability or fitness for a particular purpose.
**
** Author contact information:
**   drh@hwaci.com
**   http://www.hwaci.com/drh/
**
*******************************************************************************
**
** This module implements the delta compress algorithm.
**
** Though developed specifically for fossil, the code in this file
** is generally applicable and is thus easily separated from the
** fossil source code base.  Nothing in this file depends on anything
** else in fossil.
** 
*******************************************************************************
**
** Holepunch Note: This file has been modified for standalone use in bare-delta
*/
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <compact.h>

#include <simdle.h>

/* Remove the INTERFACE macro - Fossil uses this for its build system */
#define INTERFACE

/* Forward declare the memory functions */
void* fossil_malloc(size_t size);
void fossil_free(void* ptr);

/* Implementation of fossil memory functions */
void* fossil_malloc(size_t size) {
  return malloc(size);
}

void fossil_free(void* ptr) {
  if (ptr) {
    free(ptr);
  }
}

/* Forward declaration for the configurable delta_create */
int delta_create_with_options(
  const char *zSrc,
  size_t lenSrc,
  const char *zOut,
  size_t lenOut,
  char *zDelta,
  int nhash,
  int searchLimit
);

/*
** Macros for turning debugging printfs on and off
*/
#if 0
# define DEBUG1(X) X
#else
# define DEBUG1(X)
#endif
#if 0
#define DEBUG2(X) X
/*
** For debugging:
** Print 16 characters of text from zBuf
*/
static const char *print16(const char *z){
  int i;
  static char zBuf[20];
  for(i=0; i<16; i++){
    if( z[i]>=0x20 && z[i]<=0x7e ){
      zBuf[i] = z[i];
    }else{
      zBuf[i] = '.';
    }
  }
  zBuf[i] = 0;
  return zBuf;
}
#else
# define DEBUG2(X)
#endif

/* Always define the types we need */
typedef unsigned int u32;
typedef short int s16;
typedef unsigned short int u16;

/*
** The default width of a hash window in bytes.  The algorithm only works if this
** is a power of 2.
*/
#define NHASH_DEFAULT 16

/*
** Default search depth limit for hash collisions
*/
#define SEARCH_LIMIT_DEFAULT 64

/*
** The current state of the rolling hash.
**
** z[] holds the values that have been hashed.  z[] is a circular buffer.
** z[i] is the first entry and z[(i+nhash-1)%nhash] is the last entry of
** the window.
**
** Hash.a is the sum of all elements of hash.z[].  Hash.b is a weighted
** sum.  Hash.b is z[i]*nhash + z[i+1]*(nhash-1) + ... + z[i+nhash-1]*1.
** (Each index for z[] should be module nhash, of course.  The %nhash operator
** is omitted in the prior expression for brevity.)
*/
typedef struct hash hash;
struct hash {
  u16 a, b;         /* Hash values */
  u16 i;            /* Start of the hash window */
  u16 nhash;        /* Hash window size */
  char *z;          /* The values that have been hashed (allocated) */
};

/*
** Initialize the rolling hash using the first nhash characters of z[]
** For small hash windows, use stack allocation to avoid malloc overhead
*/
static void hash_init(hash *pHash, const char *z, int nhash){
  u16 a, b, i;
  pHash->nhash = nhash;
  
  // Use stack buffer for small hash windows to avoid malloc overhead
  if (nhash <= 32) {
    static __thread char stack_buffer[64];  // Thread-local stack buffer
    pHash->z = stack_buffer;
  } else {
    pHash->z = (char *)fossil_malloc(nhash);
  }
  
  a = b = z[0];
  for(i=1; i<nhash; i++){
    a += z[i];
    b += a;
  }
  memcpy(pHash->z, z, nhash);
  pHash->a = a & 0xffff;
  pHash->b = b & 0xffff;
  pHash->i = 0;
}

/*
** Free the rolling hash - only free heap-allocated buffers
*/
static void hash_free(hash *pHash){
  if(pHash->z && pHash->nhash > 32){
    // Only free if we used heap allocation (nhash > 32)
    fossil_free(pHash->z);
    pHash->z = 0;
  }
}

/*
** Advance the rolling hash by a single character "c"
*/
static void hash_next(hash *pHash, int c){
  u16 old = pHash->z[pHash->i];
  pHash->z[pHash->i] = c;
  pHash->i = (pHash->i+1) % pHash->nhash;
  pHash->a = pHash->a - old + c;
  pHash->b = pHash->b - pHash->nhash*old + pHash->a;
}

/*
** Return a 32-bit hash value
*/
static u32 hash_32bit(hash *pHash){
  return (pHash->a & 0xffff) | (((u32)(pHash->b & 0xffff))<<16);
}

/*
** Compute a hash on nhash bytes.
**
** This routine is intended to be equivalent to:
**    hash h;
**    hash_init(&h, zInput, nhash);
**    return hash_32bit(&h);
*/
static u32 hash_once(const char *z, int nhash){
  u16 a, b, i;
  a = b = z[0];
  for(i=1; i<nhash; i++){
    a += z[i];
    b += a;
  }
  return a | (((u32)b)<<16);
}

/*
** Write a compact-encoded integer into the given buffer.
*/
static void putInt(uint32_t v, char **pz){
  compact_state_t state;
  uintmax_t value = v;
  int err;
  
  DEBUG1( printf("putInt: encoding value %u\n", v); )
  
  // Initialize state for encoding
  state.start = 0;
  state.end = 0;
  state.buffer = (uint8_t*)*pz;
  
  // Pre-encode to calculate space needed
  err = compact_preencode_uint(&state, value);
  assert(err == 0);
  size_t needed = state.end;
  
  DEBUG1( printf("putInt: need %zu bytes for value %u\n", needed, v); )
  
  // Reset for actual encoding
  state.start = 0;
  state.end = needed;
  
  // Encode the integer using compact format
  err = compact_encode_uint(&state, value);
  assert(err == 0);
  
  DEBUG1( 
    printf("putInt: encoded %u as bytes: ", v);
    for(size_t i = 0; i < state.start; i++) {
      printf("0x%02x ", state.buffer[i]);
    }
    printf("\n");
  )
  
  // Update pointer to end of encoded data
  *pz = (char*)(state.buffer + state.start);
}

/*
** Read bytes from *pz and convert them into a positive integer.  When
** finished, leave *pz pointing to the first character past the end of
** the integer.  The *pLen parameter holds the length of the string
** in *pz and is decremented once for each character in the integer.
*/
static uint32_t getInt(const char **pz, size_t *pLen){
  compact_state_t state;
  uintmax_t result;
  int err;
  
  DEBUG1( 
    printf("getInt: decoding from %zu bytes: ", *pLen);
    for(size_t i = 0; i < (*pLen < 10 ? *pLen : 10); i++) {
      printf("0x%02x ", (uint8_t)(*pz)[i]);
    }
    printf("\n");
  )
  
  // Initialize state for decoding
  state.start = 0;
  state.end = *pLen;
  state.buffer = (uint8_t*)*pz;
  
  DEBUG1( printf("getInt: before decode - state.start=%zu, state.end=%zu\n", state.start, state.end); )
  
  // Decode the integer using compact format
  err = compact_decode_uint(&state, &result);
  
  DEBUG1( printf("getInt: after decode - err=%d, state.start=%zu, result=%ju\n", err, state.start, result); )
  
  if (err != 0) {
    // Decoding failed - return error indication
    DEBUG1( printf("getInt: decode failed with error %d\n", err); )
    return UINT32_MAX;
  }
  
  // Check for overflow - we only support 32-bit values
  if (result > UINT32_MAX) {
    DEBUG1( printf("getInt: value %ju exceeds UINT32_MAX\n", result); )
    return UINT32_MAX;
  }
  
  // Update pointer and remaining length
  size_t consumed = state.start;
  DEBUG1( printf("getInt: decoded value %u, consumed %zu bytes, new pLen will be %zu\n", (uint32_t)result, consumed, *pLen - consumed); )
  
  *pz += consumed;
  *pLen -= consumed;
  
  DEBUG1( printf("getInt: updated pointers - new *pz points to 0x%02x, *pLen=%zu\n", 
                 *pLen > 0 ? (uint8_t)(*pz)[0] : 0x00, *pLen); )
  
  return (uint32_t)result;
}

/*
** Return the number of bytes needed for compact encoding of a positive integer
*/
static int compact_size(uint32_t v){
  if (v <= 0xfc) return 1;
  if (v <= 0xffff) return 3;
  if (v <= 0xffffffff) return 5;
  return 9;
}

/*
** SIMD-optimized forward match extension using libsimdle.
** Returns the number of matching bytes starting from the given positions.
*/
static int match_forward(const char *src, const char *tgt, int maxLen) {
  int matched = 0;
  const char *srcEnd = src + maxLen;
  
  // Use libsimdle for 16-byte SIMD comparisons
  while (src + 16 <= srcEnd) {
    simdle_v128_t s_vec = simdle_load_v128_u8((const uint8_t*)src);
    simdle_v128_t t_vec = simdle_load_v128_u8((const uint8_t*)tgt);
    simdle_v128_t xor_result = simdle_xor_v128_u8(s_vec, t_vec);
    
    // Check if all bytes are zero (meaning all bytes match)
    uint64_t combined = xor_result.u64[0] | xor_result.u64[1];
    if (combined != 0) {
      // Found mismatch, use fast bit operations to find first differing byte
      if (xor_result.u64[0] != 0) {
        // Mismatch in first 8 bytes
        int byte_pos = __builtin_ctzll(xor_result.u64[0]) / 8;
        return matched + byte_pos;
      } else {
        // Mismatch in second 8 bytes  
        int byte_pos = __builtin_ctzll(xor_result.u64[1]) / 8;
        return matched + 8 + byte_pos;
      }
    }
    
    matched += 16;
    src += 16;
    tgt += 16;
  }
  
  // Handle remaining bytes  
  while (src < srcEnd && *src == *tgt) {
    matched++;
    src++;
    tgt++;
  }
  
  return matched;
}

/*
** SIMD-optimized backward match extension.
** Returns the number of matching bytes extending backwards from the given positions.
*/
static int match_backward(const char *src, const char *tgt, int maxLen) {
  int matched = 0;
  
  // Simple approach for backward matching - work backwards byte by byte
  // Could be optimized further with reverse SIMD, but complexity vs benefit
  while (matched < maxLen && src[-matched-1] == tgt[-matched-1]) {
    matched++;
  }
  
  return matched;
}


#ifdef __GNUC__
# define GCC_VERSION (__GNUC__*1000000+__GNUC_MINOR__*1000+__GNUC_PATCHLEVEL__)
#else
# define GCC_VERSION 0
#endif

/*
** Compute a 32-bit big-endian checksum on the N-byte buffer.  If the
** buffer is not a multiple of 4 bytes length, compute the sum that would
** have occurred if the buffer was padded with zeros to the next multiple
** of four bytes.
*/
static unsigned int checksum(const char *zIn, size_t N){
  static const int byteOrderTest = 1;
  const unsigned char *z = (const unsigned char *)zIn;
  unsigned sum = 0;
  if( N>0 ){
    const unsigned char *zEnd = (const unsigned char*)&zIn[N&~3];
    assert( (z - (const unsigned char*)0)%4==0 );  /* Four-byte alignment */
    if( 0==*(char*)&byteOrderTest ){
      /* This is a big-endian machine */
      while( z<zEnd ){
        sum += *(unsigned*)z;
        z += 4;
      }
    }else{
      /* A little-endian machine */
  #if GCC_VERSION>=4003000
      while( z<zEnd ){
        sum += __builtin_bswap32(*(unsigned*)z);
        z += 4;
      }
  #elif defined(_MSC_VER) && _MSC_VER>=1300
      while( z<zEnd ){
        sum += _byteswap_ulong(*(unsigned*)z);
        z += 4;
      }
  #else
      unsigned sum0 = 0;
      unsigned sum1 = 0;
      unsigned sum2 = 0;
      while(N >= 16){
        sum0 += ((unsigned)z[0] + z[4] + z[8] + z[12]);
        sum1 += ((unsigned)z[1] + z[5] + z[9] + z[13]);
        sum2 += ((unsigned)z[2] + z[6] + z[10]+ z[14]);
        sum  += ((unsigned)z[3] + z[7] + z[11]+ z[15]);
        z += 16;
        N -= 16;
      }
      while(N >= 4){
        sum0 += z[0];
        sum1 += z[1];
        sum2 += z[2];
        sum  += z[3];
        z += 4;
        N -= 4;
      }
      sum += (sum2 << 8) + (sum1 << 16) + (sum0 << 24);
  #endif
    }
    switch(N&3){
      case 3:   sum += (z[2] << 8);
      case 2:   sum += (z[1] << 16);
      case 1:   sum += (z[0] << 24);
      default:  ;
    }
  }
  return sum;
}

/*
** Create a new delta.
**
** The delta is written into a preallocated buffer, zDelta, which
** should be at least 60 bytes longer than the target file, zOut.
** The delta string will be NUL-terminated, but it might also contain
** embedded NUL characters if either the zSrc or zOut files are
** binary.  This function returns the length of the delta string
** in bytes, excluding the final NUL terminator character.
**
** Output Format:
**
** The delta begins with a compact-encoded integer.  This
** number is the number of bytes in the TARGET file.  Thus, given a
** delta file z, a program can compute the size of the output file
** simply by decoding the compact integer
** found there.  The delta_output_size() routine does exactly this.
**
** After the initial size number, the delta consists of a series of
** literal text segments and commands to copy from the SOURCE file.
** A copy command looks like this:
**
**     NNN@MMM,
**
** where NNN is the number of bytes to be copied and MMM is the offset
** into the source file of the first byte (both compact-encoded integers).   If NNN is 0
** it means copy the rest of the input file.  Literal text is like this:
**
**     NNN:TTTTT
**
** where NNN is the number of bytes of text (compact-encoded) and TTTTT is the text.
**
** The last term is of the form
**
**     NNN;
**
** In this case, NNN is a 32-bit bigendian checksum of the output file
** that can be used to verify that the delta applied correctly.  All
** numbers are compact-encoded.
**
** Pure text files generate a pure text delta.  Binary files generate a
** delta that may contain some binary data.
**
** Algorithm:
**
** The encoder first builds a hash table to help it find matching
** patterns in the source file.  16-byte chunks of the source file
** sampled at evenly spaced intervals are used to populate the hash
** table.
**
** Next we begin scanning the target file using a sliding 16-byte
** window.  The hash of the 16-byte window in the target is used to
** search for a matching section in the source file.  When a match
** is found, a copy command is added to the delta.  An effort is
** made to extend the matching section to regions that come before
** and after the 16-byte hash window.  A copy command is only issued
** if the result would use less space that just quoting the text
** literally. Literal text is added to the delta for sections that
** do not match or which can not be encoded efficiently using copy
** commands.
*/
int delta_create(
  const char *zSrc,      /* The source or pattern file */
  size_t lenSrc,         /* Length of the source file */
  const char *zOut,      /* The target file */
  size_t lenOut,         /* Length of the target file */
  char *zDelta           /* Write the delta into this buffer */
){
  return delta_create_with_options(zSrc, lenSrc, zOut, lenOut, zDelta, 
                                   NHASH_DEFAULT, SEARCH_LIMIT_DEFAULT);
}

int delta_create_with_options(
  const char *zSrc,      /* The source or pattern file */
  size_t lenSrc,         /* Length of the source file */
  const char *zOut,      /* The target file */
  size_t lenOut,         /* Length of the target file */
  char *zDelta,          /* Write the delta into this buffer */
  int nhash,             /* Hash window size (must be power of 2) */
  int searchLimit        /* Search depth limit */
){
  int i, base;
  char *zOrigDelta = zDelta;
  hash h;
  int nHash;                 /* Number of hash table entries */
  int *landmark;             /* Primary hash table */
  int *collide;              /* Collision chain */
  int lastRead = -1;         /* Last byte of zSrc read by a COPY command */

  /* Add the target file size to the beginning of the delta
  */
  putInt(lenOut, &zDelta);

  /* If the source file is very small, it means that we have no
  ** chance of ever doing a copy command.  Just output a single
  ** literal segment for the entire target and exit.
  */
  if( lenSrc<=nhash ){
    putInt(lenOut, &zDelta);
    *(zDelta++) = ':';
    memcpy(zDelta, zOut, lenOut);
    zDelta += lenOut;
    putInt(checksum(zOut, lenOut), &zDelta);
    *(zDelta++) = ';';
    return zDelta - zOrigDelta;
  }

  /* Compute the hash table used to locate matching sections in the
  ** source file.
  */
  nHash = lenSrc/nhash;
  collide = fossil_malloc( nHash*2*sizeof(int) );
  memset(collide, -1, nHash*2*sizeof(int));
  landmark = &collide[nHash];
  for(i=0; i<(int)lenSrc-nhash; i+=nhash){
    int hv = hash_once(&zSrc[i], nhash) % nHash;
    collide[i/nhash] = landmark[hv];
    landmark[hv] = i/nhash;
  }

  /* Begin scanning the target file and generating copy commands and
  ** literal sections of the delta.
  */
  base = 0;    /* We have already generated everything before zOut[base] */
  while( base+nhash<(int)lenOut ){
    int iSrc, iBlock;
    unsigned int bestCnt, bestOfst=0, bestLitsz=0;
    hash_init(&h, &zOut[base], nhash);
    i = 0;     /* Trying to match a landmark against zOut[base+i] */
    bestCnt = 0;
    while( 1 ){
      int hv;
      int limit = searchLimit;

      hv = hash_32bit(&h) % nHash;
      DEBUG2( printf("LOOKING: %4d [%s]\n", base+i, print16(&zOut[base+i])); )
      iBlock = landmark[hv];
      while( iBlock>=0 && (limit--)>0 ){
        /*
        ** The hash window has identified a potential match against
        ** landmark block iBlock.  But we need to investigate further.
        **
        ** Look for a region in zOut that matches zSrc. Anchor the search
        ** at zSrc[iSrc] and zOut[base+i].  Do not include anything prior to
        ** zOut[base] or after zOut[outLen] nor anything after zSrc[srcLen].
        **
        ** Set cnt equal to the length of the match and set ofst so that
        ** zSrc[ofst] is the first element of the match.  litsz is the number
        ** of characters between zOut[base] and the beginning of the match.
        ** sz will be the overhead (in bytes) needed to encode the copy
        ** command.  Only generate copy command if the overhead of the
        ** copy command is less than the amount of literal text to be copied.
        */
        int cnt, ofst, litsz;
        int j, k, x, y;
        int sz;
        int limitX;

        /* Get candidate source position from hash table */
        iSrc = iBlock*nhash;
        y = base+i;
        
        /* FIRST: Verify the hash window actually matches (eliminate hash collisions) */
        if (memcmp(&zSrc[iSrc], &zOut[y], nhash) != 0) {
          /* Hash collision - skip this block */
          iBlock = collide[iBlock];
          continue;
        }
        
        /* SECOND: Extend forward from END of verified hash window */
        int forward_start_src = iSrc + nhash;
        int forward_start_tgt = y + nhash;
        int max_forward = (lenSrc - forward_start_src < lenOut - forward_start_tgt) 
                         ? lenSrc - forward_start_src 
                         : lenOut - forward_start_tgt;
        j = (max_forward > 0) ? match_forward(&zSrc[forward_start_src], &zOut[forward_start_tgt], max_forward) : 0;
        
        /* THIRD: Extend backward from START of verified hash window */
        int max_backward = (iSrc < i) ? iSrc : i;
        k = (max_backward > 0) ? match_backward(&zSrc[iSrc], &zOut[y], max_backward) : 0;
        
        /* FOURTH: Compute final match region (now guaranteed correct) */
        ofst = iSrc - k;
        cnt = k + nhash + j;  /* backward + verified_window + forward */
        litsz = i - k;  /* Number of bytes of literal text before the copy */
        DEBUG2( printf("MATCH %d bytes at %d: [%s] litsz=%d\n",
                        cnt, ofst, print16(&zSrc[ofst]), litsz); )
        /* sz will hold the number of bytes needed to encode the "insert"
        ** command and the copy command, not counting the "insert" text */
        sz = compact_size(i-k)+compact_size(cnt)+compact_size(ofst)+3;
        if( cnt>=sz && cnt>(int)bestCnt ){
          /* Remember this match only if it is the best so far and it
          ** does not increase the file size */
          bestCnt = cnt;
          bestOfst = iSrc-k;
          bestLitsz = litsz;
          DEBUG2( printf("... BEST SO FAR\n"); )
        }

        /* Check the next matching block */
        iBlock = collide[iBlock];
      }

      /* We have a copy command that does not cause the delta to be larger
      ** than a literal insert.  So add the copy command to the delta.
      */
      if( bestCnt>0 ){
        if( bestLitsz>0 ){
          /* Add an insert command before the copy */
          putInt(bestLitsz,&zDelta);
          *(zDelta++) = ':';
          memcpy(zDelta, &zOut[base], bestLitsz);
          zDelta += bestLitsz;
          base += bestLitsz;
          DEBUG2( printf("insert %d\n", bestLitsz); )
        }
        base += bestCnt;
        putInt(bestCnt, &zDelta);
        *(zDelta++) = '@';
        putInt(bestOfst, &zDelta);
        DEBUG2( printf("copy %d bytes from %d\n", bestCnt, bestOfst); )
        *(zDelta++) = ',';
        if( (int)(bestOfst + bestCnt -1) > lastRead ){
          lastRead = bestOfst + bestCnt - 1;
          DEBUG2( printf("lastRead becomes %d\n", lastRead); )
        }
        bestCnt = 0;
        break;
      }

      /* If we reach this point, it means no match is found so far */
      if( base+i+nhash>=(int)lenOut ){
        /* We have reached the end of the file and have not found any
        ** matches.  Do an "insert" for everything that does not match */
        putInt(lenOut-base, &zDelta);
        *(zDelta++) = ':';
        memcpy(zDelta, &zOut[base], lenOut-base);
        zDelta += lenOut-base;
        base = lenOut;
        break;
      }

      /* Advance the hash by one character.  Keep looking for a match */
      hash_next(&h, zOut[base+i+nhash]);
      i++;
    }
  }
  
  /* Clean up hash */
  hash_free(&h);
  /* Output a final "insert" record to get all the text at the end of
  ** the file that does not match anything in the source file.
  */
  if( base<(int)lenOut ){
    putInt(lenOut-base, &zDelta);
    *(zDelta++) = ':';
    memcpy(zDelta, &zOut[base], lenOut-base);
    zDelta += lenOut-base;
  }
  /* Output the final checksum record. */
  putInt(checksum(zOut, lenOut), &zDelta);
  *(zDelta++) = ';';
  fossil_free(collide);
  return zDelta - zOrigDelta;
}

/*
** Return the size (in bytes) of the output from applying
** a delta.
**
** This routine is provided so that an procedure that is able
** to call delta_apply() can learn how much space is required
** for the output and hence allocate nor more space that is really
** needed.
*/
int delta_output_size(const char *zDelta, size_t lenDelta){
  uint32_t size;
  size = getInt(&zDelta, &lenDelta);
  if( size == UINT32_MAX ){
    /* ERROR: failed to decode size integer */
    return -1;
  }
  return (int)size;
}


/*
** Apply a delta.
**
** The output buffer should be big enough to hold the whole output
** file and a NUL terminator at the end.  The delta_output_size()
** routine will determine this size for you.
**
** The delta string should be null-terminated.  But the delta string
** may contain embedded NUL characters (if the input and output are
** binary files) so we also have to pass in the length of the delta in
** the lenDelta parameter.
**
** This function returns the size of the output file in bytes (excluding
** the final NUL terminator character).  Except, if the delta string is
** malformed or intended for use with a source file other than zSrc,
** then this routine returns -1.
**
** Refer to the delta_create() documentation above for a description
** of the delta file format.
*/
int delta_apply(
  const char *zSrc,      /* The source or pattern file */
  size_t lenSrc,         /* Length of the source file */
  const char *zDelta,    /* Delta to apply to the pattern */
  size_t lenDelta,       /* Length of the delta */
  char *zOut             /* Write the output into this preallocated buffer */
){
  uint32_t limit;
  uint32_t total = 0;
#ifdef FOSSIL_ENABLE_DELTA_CKSUM_TEST
  char *zOrigOut = zOut;
#endif

  limit = getInt(&zDelta, &lenDelta);
  if( limit == UINT32_MAX ){
    /* ERROR: failed to decode size integer */
    DEBUG1( printf("delta_apply: ERROR - failed to decode target size\n"); )
    return -1;
  }
  DEBUG1( printf("delta_apply: target size = %u, remaining delta = %zu bytes\n", limit, lenDelta); )
  while( lenDelta>0 ){
    uint32_t cnt, ofst;
    
    DEBUG1( printf("delta_apply: loop iteration, %zu bytes remaining, first byte = 0x%02x\n", lenDelta, (uint8_t)*zDelta); )
    
    cnt = getInt(&zDelta, &lenDelta);
    
    if (cnt == UINT32_MAX) {
      DEBUG1( printf("delta_apply: ERROR - failed to decode operation count\n"); )
      return -1;
    }
    
    DEBUG1( printf("delta_apply: operation count = %u, next char = 0x%02x ('%c')\n", 
                   cnt, (uint8_t)zDelta[0], zDelta[0] >= 32 && zDelta[0] <= 126 ? zDelta[0] : '?'); )
    
    switch( zDelta[0] ){
      case '@': {
        zDelta++; lenDelta--;
        ofst = getInt(&zDelta, &lenDelta);
        if( lenDelta>0 && zDelta[0]!=',' ){
          /* ERROR: copy command not terminated by ',' */
          return -1;
        }
        zDelta++; lenDelta--;
        DEBUG1( printf("COPY %d from %d\n", cnt, ofst); )
        total += cnt;
        if( total>limit ){
          /* ERROR: copy exceeds output file size */
          return -1;
        }
        if( (int)(ofst+cnt) > lenSrc ){
          /* ERROR: copy extends past end of input */
          return -1;
        }
        memcpy(zOut, &zSrc[ofst], cnt);
        zOut += cnt;
        break;
      }
      case ':': {
        zDelta++; lenDelta--;
        total += cnt;
        if( total>limit ){
          /* ERROR:  insert command gives an output larger than predicted */
          DEBUG1( printf("delta_apply: ERROR - insert would exceed limit (%u > %u)\n", total, limit); )
          return -1;
        }
        DEBUG1( printf("delta_apply: INSERT %u bytes (total now %u/%u)\n", cnt, total, limit); )
        if( (int)cnt>lenDelta ){
          /* ERROR: insert count exceeds size of delta */
          DEBUG1( printf("delta_apply: ERROR - insert count %u exceeds remaining delta %zu\n", cnt, lenDelta); )
          return -1;
        }
        if (cnt > 0) {
          memcpy(zOut, zDelta, cnt);
          zOut += cnt;
        }
        zDelta += cnt;
        lenDelta -= cnt;
        DEBUG1( printf("delta_apply: INSERT completed, %zu bytes remaining in delta\n", lenDelta); )
        break;
      }
      case ';': {
        zDelta++; lenDelta--;
        zOut[0] = 0;
#ifdef FOSSIL_ENABLE_DELTA_CKSUM_TEST
        if( cnt!=checksum(zOrigOut, total) ){
          /* ERROR:  bad checksum */
          DEBUG1( printf("delta_apply: ERROR - bad checksum\n"); )
          return -1;
        }
#endif
        if( total!=limit ){
          /* ERROR: generated size does not match predicted size */
          DEBUG1( printf("delta_apply: ERROR - size mismatch: generated %u != predicted %u\n", total, limit); )
          return -1;
        }
        return total;
      }
      default: {
        /* ERROR: unknown delta operator */
        return -1;
      }
    }
  }
  /* ERROR: unterminated delta */
  return -1;
}

/*
** Analyze a delta.  Figure out the total number of bytes copied from
** source to target, and the total number of bytes inserted by the delta,
** and return both numbers.
*/
int delta_analyze(
  const char *zDelta,    /* Delta to apply to the pattern */
  size_t lenDelta,       /* Length of the delta */
  int *pnCopy,           /* OUT: Number of bytes copied */
  int *pnInsert          /* OUT: Number of bytes inserted */
){
  uint32_t nInsert = 0;
  uint32_t nCopy = 0;

  uint32_t size = getInt(&zDelta, &lenDelta);
  if( size == UINT32_MAX ){
    /* ERROR: failed to decode size integer */
    return -1;
  }
  while( lenDelta>0 ){
    uint32_t cnt;
    cnt = getInt(&zDelta, &lenDelta);
    switch( zDelta[0] ){
      case '@': {
        zDelta++; lenDelta--;
        (void)getInt(&zDelta, &lenDelta);
        if( lenDelta>0 && zDelta[0]!=',' ){
          /* ERROR: copy command not terminated by ',' */
          return -1;
        }
        zDelta++; lenDelta--;
        nCopy += cnt;
        break;
      }
      case ':': {
        zDelta++; lenDelta--;
        nInsert += cnt;
        if( (int)cnt>lenDelta ){
          /* ERROR: insert count exceeds size of delta */
          return -1;
        }
        zDelta += cnt;
        lenDelta -= cnt;
        break;
      }
      case ';': {
        *pnCopy = nCopy;
        *pnInsert = nInsert;
        return 0;
      }
      default: {
        /* ERROR: unknown delta operator */
        return -1;
      }
    }
  }
  /* ERROR: unterminated delta */
  return -1;
}
