#include "experimentalavxbase64.h"

#include <x86intrin.h>
#include <stdbool.h>

/**
* This code borrows from Wojciech Mula's library at
* https://github.com/WojciechMula/base64simd (published under BSD)
* as well as code from Alfred Klomp's library https://github.com/aklomp/base64 (published under BSD)
*
*/




/**
* Note : Hardware such as Knights Landing might do poorly with this AVX2 code since it relies on shuffles. Alternatives might be faster.
*/


static inline __m256i _mm256_bswap_epi32(const __m256i in) {
  // _mm256_shuffle_epi8() works on two 128-bit lanes separately:
  return _mm256_shuffle_epi8(in, _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11,
                                                  10, 9, 8, 15, 14, 13, 12, 3,
                                                  2, 1, 0, 7, 6, 5, 4, 11, 10,
                                                  9, 8, 15, 14, 13, 12));
}

static inline __m256i enc_reshuffle(const __m256i input) {

    // translation from SSE into AVX2 of procedure
    // https://github.com/WojciechMula/base64simd/blob/master/encode/unpack_bigendian.cpp
    const __m256i in = _mm256_shuffle_epi8(input, _mm256_set_epi8(
        10, 11,  9, 10,
         7,  8,  6,  7,
         4,  5,  3,  4,
         1,  2,  0,  1,

        14, 15, 13, 14,
        11, 12, 10, 11,
         8,  9,  7,  8,
         5,  6,  4,  5
    ));

    const __m256i t0 = _mm256_and_si256(in, _mm256_set1_epi32(0x0fc0fc00));
    const __m256i t1 = _mm256_mulhi_epu16(t0, _mm256_set1_epi32(0x04000040));

    const __m256i t2 = _mm256_and_si256(in, _mm256_set1_epi32(0x003f03f0));
    const __m256i t3 = _mm256_mullo_epi16(t2, _mm256_set1_epi32(0x01000010));

    return _mm256_or_si256(t1, t3);
}

static inline __m256i enc_translate(const __m256i in) {
  const __m256i lut = _mm256_setr_epi8(
      65, 71, -4, -4, -4, -4, -4, -4, -4, -4, -4, -4, -19, -16, 0, 0, 65, 71,
      -4, -4, -4, -4, -4, -4, -4, -4, -4, -4, -19, -16, 0, 0);
  __m256i indices = _mm256_subs_epu8(in, _mm256_set1_epi8(51));
  __m256i mask = _mm256_cmpgt_epi8((in), _mm256_set1_epi8(25));
  indices = _mm256_sub_epi8(indices, mask);
  __m256i out = _mm256_add_epi8(in, _mm256_shuffle_epi8(lut, indices));
  return out;
}

static inline __m256i dec_reshuffle(__m256i in) {

  // inlined procedure pack_madd from https://github.com/WojciechMula/base64simd/blob/master/decode/pack.avx2.cpp
  // The only difference is that elements are reversed,
  // only the multiplication constants were changed.

  const __m256i merge_ab_and_bc = _mm256_maddubs_epi16(in, _mm256_set1_epi32(0x01400140)); //_mm256_maddubs_epi16 is likely expensive
  __m256i out = _mm256_madd_epi16(merge_ab_and_bc, _mm256_set1_epi32(0x00011000));
  // end of inlined

  // Pack bytes together within 32-bit words, discarding words 3 and 7:
  out = _mm256_shuffle_epi8(out, _mm256_setr_epi8(
        2, 1, 0, 6, 5, 4, 10, 9, 8, 14, 13, 12, -1, -1, -1, -1,
        2, 1, 0, 6, 5, 4, 10, 9, 8, 14, 13, 12, -1, -1, -1, -1
  ));
  // the call to _mm256_permutevar8x32_epi32 could be replaced by a call to _mm256_storeu2_m128i but it is doubtful that it would help
  return _mm256_permutevar8x32_epi32(
      out, _mm256_setr_epi32(0, 1, 2, 4, 5, 6, -1, -1));
}


size_t expavx2_base64_encode(char* dest, const char* str, size_t len) {
      const char* const dest_orig = dest;
      if(len >= 32 - 4) {
        // first load is masked
        __m256i inputvector = _mm256_maskload_epi32((int const*)(str - 4),  _mm256_set_epi32(
            0x80000000,
            0x80000000,
            0x80000000,
            0x80000000,

            0x80000000,
            0x80000000,
            0x80000000,
            0x00000000 // we do not load the first 4 bytes
        ));
        //////////
        // Intel docs: Faults occur only due to mask-bit required memory accesses that caused the faults.
        // Faults will not occur due to referencing any memory location if the corresponding mask bit for
        //that memory location is 0. For example, no faults will be detected if the mask bits are all zero.
        ////////////
        while(true) {
          inputvector = enc_reshuffle(inputvector);
          inputvector = enc_translate(inputvector);
          _mm256_storeu_si256((__m256i *)dest, inputvector);
          str += 24;
          dest += 32;
          len -= 24;
          if(len >= 32) {
            inputvector = _mm256_loadu_si256((__m256i *)(str - 4)); // no need for a mask here
            // we could do a mask load as long as len >= 24
          } else {
            break;
          }
        }
      }
      size_t scalarret = chromium_base64_encode(dest, str, len);
      if(scalarret == MODP_B64_ERROR) return MODP_B64_ERROR;
      return (dest - dest_orig) + scalarret;
}

size_t expavx2_base64_decode(char *out, const char *src, size_t srclen) {
      char* out_orig = out;
      while (srclen >= 45) {

        // The input consists of six character sets in the Base64 alphabet,
        // which we need to map back to the 6-bit values they represent.
        // There are three ranges, two singles, and then there's the rest.
        //
        //  #  From       To        Add  Characters
        //  1  [43]       [62]      +19  +
        //  2  [47]       [63]      +16  /
        //  3  [48..57]   [52..61]   +4  0..9
        //  4  [65..90]   [0..25]   -65  A..Z
        //  5  [97..122]  [26..51]  -71  a..z
        // (6) Everything else => invalid input

        __m256i str = _mm256_loadu_si256((__m256i *)src);

        // inlined function lookup_pshufb_bitmask from https://github.com/WojciechMula/base64simd/blob/master/decode/lookup.avx2.cpp
        // http://0x80.pl/notesen/2016-01-17-sse-base64-decoding.html#lookup-pshufb-bitmask

        const __m256i higher_nibble = _mm256_and_si256(_mm256_srli_epi32(str, 4), _mm256_set1_epi8(0x0f));
        const __m256i lower_nibble  = _mm256_and_si256(str, _mm256_set1_epi8(0x0f));

        const __m256i shiftLUT = _mm256_setr_epi8(
            0,   0,  19,   4, -65, -65, -71, -71,
            0,   0,   0,   0,   0,   0,   0,   0,

            0,   0,  19,   4, -65, -65, -71, -71,
            0,   0,   0,   0,   0,   0,   0,   0
        );

        const __m256i maskLUT  = _mm256_setr_epi8(
            /* 0        */ (char)(0b10101000),
            /* 1 .. 9   */ (char)(0b11111000), (char)(0b11111000), (char)(0b11111000), (char)(0b11111000),
                           (char)(0b11111000), (char)(0b11111000), (char)(0b11111000), (char)(0b11111000),
                           (char)(0b11111000),
            /* 10       */ (char)(0b11110000),
            /* 11       */ (char)(0b01010100),
            /* 12 .. 14 */ (char)(0b01010000), (char)(0b01010000), (char)(0b01010000),
            /* 15       */ (char)(0b01010100),

            /* 0        */ (char)(0b10101000),
            /* 1 .. 9   */ (char)(0b11111000), (char)(0b11111000), (char)(0b11111000), (char)(0b11111000),
                           (char)(0b11111000), (char)(0b11111000), (char)(0b11111000), (char)(0b11111000),
                           (char)(0b11111000),
            /* 10       */ (char)(0b11110000),
            /* 11       */ (char)(0b01010100),
            /* 12 .. 14 */ (char)(0b01010000), (char)(0b01010000), (char)(0b01010000),
            /* 15       */ (char)(0b01010100)
        );

        const __m256i bitposLUT = _mm256_setr_epi8(
            0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, (char)(0x80),
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

            0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, (char)(0x80),
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        );

        const __m256i sh     = _mm256_shuffle_epi8(shiftLUT,  higher_nibble);
        const __m256i eq_2f  = _mm256_cmpeq_epi8(str, _mm256_set1_epi8(0x2f));
        const __m256i shift  = _mm256_blendv_epi8(sh, _mm256_set1_epi8(16), eq_2f);

        const __m256i M      = _mm256_shuffle_epi8(maskLUT,   lower_nibble);
        const __m256i bit    = _mm256_shuffle_epi8(bitposLUT, higher_nibble);

        const __m256i non_match = _mm256_cmpeq_epi8(_mm256_and_si256(M, bit), _mm256_setzero_si256());

        if (_mm256_movemask_epi8(non_match)) {
            break;
        }
        srclen -= 32;
        src += 32;

        str = _mm256_add_epi8(str, shift);

        // end of inlined function

        // Reshuffle the input to packed 12-byte output format:
        str = dec_reshuffle(str);
        _mm256_storeu_si256((__m256i *)out, str);
        out += 24;
      }
      size_t scalarret = chromium_base64_decode(out, src, srclen);
      if(scalarret == MODP_B64_ERROR) return MODP_B64_ERROR;
      return (out - out_orig) + scalarret;
}
