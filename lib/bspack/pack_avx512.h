#ifndef BSPACK_INTERNAL
#error "this file is a section of bspack.c and is compiled only through it"
#endif
/* ==== pack API, AVX-512 (GFNI + VBMI) ===================================
 *
 * Where pack_portable.h reaches the same result with the six exchange
 * stages of a recursive-halving transpose, here the whole masks <-> planes
 * conversion is a tiled bit-matrix transpose built from three primitives,
 * none of which the compiler finds on its own:
 *
 *  - vgf2p8affineqb transposes an 8x8 bit tile per qword: 8 hands x 8
 *    cards in one instruction: the intra-byte stages 4/2/1 plus the
 *    byte-boundary stage in one shot.  The
 *    hardware convention is
 *        transpose8x8(q) = gf2p8affine(src1 = 0x8040201008040201,
 *                                      src2 = bswap64(q), imm = 0)
 *    i.e. the second operand is the matrix with its rows in reversed
 *    byte order -- the bswap is folded into the vpermb that precedes
 *    every affine below (P1R = byte-grid transpose composed with the
 *    per-qword byte reverse).
 *  - vpermb (byte grid transpose within a zmm) replaces stages 8/16/32
 *    on the byte side.
 *  - vpermi2q pairs (qtrn8: an across-register 8x8 qword transpose,
 *    three butterfly levels of two-source permutes) do the cross-block
 *    redistribution that the batch layout requires: a batch is BS_BATCH
 *    contiguous per-hand u64s, but hand 64w+i belongs in lane w of row i.
 *
 * Pack runs in two passes over a 4 KB scratch: pass 1 per 64-hand block
 * produces plane rows in block-local lanes, pass 2 per 8-plane group
 * transposes blocks into lane position.  Unpack is the mirror image,
 * with the u32 narrowing done as byte-stream interleaves (three
 * two-source byte permutes build four output vectors from the <=4
 * value-byte planes; missing byte planes are zero registers, so
 * nout < 32 needs no masking anywhere). */

#include <immintrin.h>

/* byte-grid transpose: byte 8a+b <- 8b+a */
static const unsigned char P1_IDX[64] = {
     0,  8, 16, 24, 32, 40, 48, 56,
     1,  9, 17, 25, 33, 41, 49, 57,
     2, 10, 18, 26, 34, 42, 50, 58,
     3, 11, 19, 27, 35, 43, 51, 59,
     4, 12, 20, 28, 36, 44, 52, 60,
     5, 13, 21, 29, 37, 45, 53, 61,
     6, 14, 22, 30, 38, 46, 54, 62,
     7, 15, 23, 31, 39, 47, 55, 63,
};

/* the same composed with a per-qword byte reverse (gf2p8affine's matrix
 * operand convention): byte 8a+b <- 8(7-b)+a */
static const unsigned char P1R_IDX[64] = {
    56, 48, 40, 32, 24, 16,  8,  0,
    57, 49, 41, 33, 25, 17,  9,  1,
    58, 50, 42, 34, 26, 18, 10,  2,
    59, 51, 43, 35, 27, 19, 11,  3,
    60, 52, 44, 36, 28, 20, 12,  4,
    61, 53, 45, 37, 29, 21, 13,  5,
    62, 54, 46, 38, 30, 22, 14,  6,
    63, 55, 47, 39, 31, 23, 15,  7,
};

#define TR8 _mm512_set1_epi64((long long)0x8040201008040201ull)

/* across-register 8x8 qword transpose: r[k].qword[j] <- r[j].qword[k].
 * Three butterfly levels; level l swaps bit l of the register index with
 * bit l of the qword index, one level per two-source permute pair. */
static inline void qtrn8(__m512i r[8])
{
    const __m512i l0a = _mm512_setr_epi64(0, 8, 2, 10, 4, 12, 6, 14);
    const __m512i l0b = _mm512_setr_epi64(1, 9, 3, 11, 5, 13, 7, 15);
    const __m512i l1a = _mm512_setr_epi64(0, 1, 8, 9, 4, 5, 12, 13);
    const __m512i l1b = _mm512_setr_epi64(2, 3, 10, 11, 6, 7, 14, 15);
    const __m512i l2a = _mm512_setr_epi64(0, 1, 2, 3, 8, 9, 10, 11);
    const __m512i l2b = _mm512_setr_epi64(4, 5, 6, 7, 12, 13, 14, 15);
#pragma GCC unroll 4
    for (int j = 0; j < 8; j += 2) {
        __m512i a = r[j], b = r[j + 1];
        r[j]     = _mm512_permutex2var_epi64(a, l0a, b);
        r[j + 1] = _mm512_permutex2var_epi64(a, l0b, b);
    }
#pragma GCC unroll 2
    for (int g = 0; g < 8; g += 4)
#pragma GCC unroll 2
        for (int j = g; j < g + 2; j++) {
            __m512i a = r[j], b = r[j + 2];
            r[j]     = _mm512_permutex2var_epi64(a, l1a, b);
            r[j + 2] = _mm512_permutex2var_epi64(a, l1b, b);
        }
#pragma GCC unroll 4
    for (int j = 0; j < 4; j++) {
        __m512i a = r[j], b = r[j + 4];
        r[j]     = _mm512_permutex2var_epi64(a, l2a, b);
        r[j + 4] = _mm512_permutex2var_epi64(a, l2b, b);
    }
}

/* Pack pass 1, per 64-hand block w: load the block's masks, tile-transpose
 * (vpermb + affine), then regroup so scr[8w+B] holds plane rows 8B..8B+7
 * of block w: scr[8w+B].qword[e] = bits of card 8B+e across hands
 * 64w..64w+63.  Pass 2 (in the callers) transposes across w so each plane
 * row lands in lane w of its plane word.  nB = card octets kept: 8 for
 * bs_transpose (all 64 planes, junk 52..63 included per the contract), 7
 * for bs_transpose_map (cards 56..63 have no map slot; constant per call
 * site, so the tail work folds away). */
static inline void pack_pass1(const uint64_t *m, __m512i scr[64], int nB)
{
    const __m512i p1  = _mm512_loadu_si512(P1_IDX);
    const __m512i p1r = _mm512_loadu_si512(P1R_IDX);
    for (int w = 0; w < 8; w++) {
        __m512i r[8];
#pragma GCC unroll 8
        for (int k = 0; k < 8; k++) {
            __m512i z = _mm512_loadu_si512(m + 64 * w + 8 * k);
            z = _mm512_permutexvar_epi8(p1r, z);
            r[k] = _mm512_gf2p8affine_epi64_epi8(TR8, z, 0);
        }
        qtrn8(r);
#pragma GCC unroll 8
        for (int B = 0; B < nB; B++)
            scr[8 * w + B] = _mm512_permutexvar_epi8(p1, r[B]);
    }
}

/* use_map 0: write all 64 planes in place (bs_transpose's contract).
 * use_map 1: write plane c to planes[map[c]], c < 52 only -- so pass 1 fills
 * seven byte groups, not eight.  always_inline, and use_map/nB are literals
 * at both call sites, so the unused arm and the c < 52 tests fold away.
 * (Branching on map != NULL instead would not fold: map is a runtime
 * parameter of bs_transpose_map, costing a test per store group.) */
static inline __attribute__((always_inline)) void
pack_fused(const uint64_t masks[BS_BATCH], const int *map, bs_word *planes,
           int nB, int use_map)
{
    __m512i scr[64];
    pack_pass1(masks, scr, nB);
    for (int B = 0; B < nB; B++) {
        __m512i r[8];
#pragma GCC unroll 8
        for (int w = 0; w < 8; w++) r[w] = scr[8 * w + B];
        qtrn8(r);
#pragma GCC unroll 8
        for (int e = 0; e < 8; e++) {
            const int c = 8 * B + e;
            if (!use_map)
                _mm512_store_si512((__m512i *)&planes[c], r[e]);
            else if (c < 52)
                _mm512_store_si512((__m512i *)&planes[map[c]], r[e]);
        }
    }
}

void bs_transpose(const uint64_t masks[BS_BATCH], bs_word planes[64])
{
    pack_fused(masks, NULL, planes, 8, 0);
}

void bs_transpose_map(const uint64_t masks[BS_BATCH], const int map[52],
                      bs_word *planes)
{
    pack_fused(masks, map, planes, 7, 1);
}

void bs_untranspose(const bs_word *planes, int nout, uint32_t vals[BS_BATCH])
{
    /* byte-stream interleave indices for the finale: IDX01_t picks value
     * byte 0 (from source a) and byte 1 (from source b) of 16 consecutive
     * hands t*16..t*16+15; IDX23 merges two such pair-streams into the
     * final u32 layout. */
    static const unsigned char IDX01[4][64] = {
      {  0, 64, 0, 0,  1, 65, 0, 0,  2, 66, 0, 0,  3, 67, 0, 0,
         4, 68, 0, 0,  5, 69, 0, 0,  6, 70, 0, 0,  7, 71, 0, 0,
         8, 72, 0, 0,  9, 73, 0, 0, 10, 74, 0, 0, 11, 75, 0, 0,
        12, 76, 0, 0, 13, 77, 0, 0, 14, 78, 0, 0, 15, 79, 0, 0 },
      { 16, 80, 0, 0, 17, 81, 0, 0, 18, 82, 0, 0, 19, 83, 0, 0,
        20, 84, 0, 0, 21, 85, 0, 0, 22, 86, 0, 0, 23, 87, 0, 0,
        24, 88, 0, 0, 25, 89, 0, 0, 26, 90, 0, 0, 27, 91, 0, 0,
        28, 92, 0, 0, 29, 93, 0, 0, 30, 94, 0, 0, 31, 95, 0, 0 },
      { 32,  96, 0, 0, 33,  97, 0, 0, 34,  98, 0, 0, 35,  99, 0, 0,
        36, 100, 0, 0, 37, 101, 0, 0, 38, 102, 0, 0, 39, 103, 0, 0,
        40, 104, 0, 0, 41, 105, 0, 0, 42, 106, 0, 0, 43, 107, 0, 0,
        44, 108, 0, 0, 45, 109, 0, 0, 46, 110, 0, 0, 47, 111, 0, 0 },
      { 48, 112, 0, 0, 49, 113, 0, 0, 50, 114, 0, 0, 51, 115, 0, 0,
        52, 116, 0, 0, 53, 117, 0, 0, 54, 118, 0, 0, 55, 119, 0, 0,
        56, 120, 0, 0, 57, 121, 0, 0, 58, 122, 0, 0, 59, 123, 0, 0,
        60, 124, 0, 0, 61, 125, 0, 0, 62, 126, 0, 0, 63, 127, 0, 0 },
    };
    static const unsigned char IDX23_TBL[64] = {
         0,  1, 64, 65,  4,  5,  68,  69,  8,  9, 72, 73, 12, 13,  76,  77,
        16, 17, 80, 81, 20, 21,  84,  85, 24, 25, 88, 89, 28, 29,  92,  93,
        32, 33, 96, 97, 36, 37, 100, 101, 40, 41, 104, 105, 44, 45, 108, 109,
        48, 49, 112, 113, 52, 53, 116, 117, 56, 57, 120, 121, 60, 61, 124, 125,
    };
    const __m512i p1r  = _mm512_loadu_si512(P1R_IDX);
    const __m512i zero = _mm512_setzero_si512();
    const int nb = (nout + 7) >> 3;

    /* g[K][d] = value byte K of hands 64d..64d+63 (one byte per hand) */
    __m512i g[4][8];
    for (int K = 0; K < nb; K++) {
        __m512i r[8];
#pragma GCC unroll 8
        for (int j = 0; j < 8; j++)
            r[j] = 8 * K + j < nout
                       ? _mm512_loadu_si512(&planes[8 * K + j])
                       : zero;
        qtrn8(r);
#pragma GCC unroll 8
        for (int d = 0; d < 8; d++) {
            __m512i v = _mm512_permutexvar_epi8(p1r, r[d]);
            g[K][d] = _mm512_gf2p8affine_epi64_epi8(TR8, v, 0);
        }
    }
    for (int K = nb; K < 4; K++)
        for (int d = 0; d < 8; d++) g[K][d] = zero;

    for (int d = 0; d < 8; d++)
#pragma GCC unroll 4
        for (int t = 0; t < 4; t++) {
            __m512i i01 = _mm512_loadu_si512(IDX01[t]);
            __m512i x = _mm512_permutex2var_epi8(g[0][d], i01, g[1][d]);
            __m512i y = _mm512_permutex2var_epi8(g[2][d], i01, g[3][d]);
            __m512i o = _mm512_permutex2var_epi8(
                x, _mm512_loadu_si512(IDX23_TBL), y);
            _mm512_storeu_si512(vals + 64 * d + 16 * t, o);
        }
}
