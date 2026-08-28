#ifndef BSPACK_INTERNAL
#error "this file is a section of bspack.c and is compiled only through it"
#endif
/* ==== pack API, portable ==============================================
 *
 * Follows transpose64_portable.h, whose masks, SW / SW0 and LADDER421
 * macros it uses.  Implements
 * bs_transpose / bs_transpose_map / bs_untranspose for every target with
 * no instruction-set-specific core, and for PACK=portable anywhere.  Same
 * dialect as the transpose above it: GNU vector operators and
 * __builtin_shufflevector, no immintrin.h and no arm_neon.h.
 *
 * The transpose's six exchange stages split into two passes, and the order
 * they run in decides what else can fold into them.  Stages 4/2/1 group
 * rows into contiguous octets; stages 32/16/8 group them into stride-8
 * sets.  The batch gather produces eight CONSECUTIVE rows at a time and
 * the plane store consumes whatever the following pass holds, so the octet
 * pass runs first with the gather folded into it, and the stride-8 pass
 * runs second with the store folded into it.  The 64-row buffer between
 * them is written once and read once.
 *
 * bs_untranspose transposes 32 rows, not 64.  nout <= 32 (bsapi.h), so
 * only the low half of each output word survives to vals[], and the
 * 64-bit stage is dropped: what that stage would have done is set row
 * 32+k to row k >> 32.  Every remaining stage is local to a 32-bit half of
 * the word -- (x >> J) & M for J in {16,8,4,2,1} with these masks moves no
 * bit across bit 31 -- so rows 32..63 stay equal to rows 0..31 >> 32 for
 * the whole transpose and are never computed.  The scatter reads the high
 * half of row k instead.
 *
 * With stages 4/2/1 first, the octets that pass sees are octets of PLANES,
 * of which only ceil(nout/8) hold anything and the rest are zero.  ng is a
 * literal at every call site, so wherever the group loop is unrolled the
 * empty groups fold out at compile time; at four lanes that loop is left
 * rolled for its size and they are skipped at run time instead.  The
 * stride-8 sets of the second pass still enter with zero rows, and a swap
 * against a known-zero partner is SW0's four operations rather than SW's
 * six.  So an 8-plane call costs well under a 24-plane one.
 *
 * At four lanes a second spelling takes over for the byte-granular work:
 * stages 32/16/8 over a stride-8 row set are exactly a per-lane 8x8 byte
 * transpose, which
 * __builtin_shufflevector does in 24 shuffles against 72 shift/logic
 * operations for the masked swaps, and bs_untranspose then assembles a u32
 * value out of four byte planes with widening interleaves that also put
 * the hands in order -- one pass, no row buffer.  The shuffle patterns are
 * pairwise within each 16-byte half, which is the interleave wide vector
 * units provide; a target whose shuffle unit is narrower than its vector
 * width will spend more slots on them than on the masked swaps they
 * replace.
 */

/* ---- the four-lane byte-shuffle building blocks -------------------------- */

#if BS_LANES == 4
typedef uint8_t  bs_u8v  __attribute__((vector_size(32)));
typedef uint16_t bs_u16v __attribute__((vector_size(32)));
typedef uint32_t bs_u32v __attribute__((vector_size(32)));
typedef uint64_t bs_u64v __attribute__((vector_size(32)));

/* Pairwise interleave within each 16-byte half, at four element widths,
 * plus the 128-bit lane crossing.  Each index list names one instruction
 * on a 32-byte vector unit: on x86 the four interleaves are
 * vpunpck{l,h}{bw,wd,dq,qdq} and the crossing is vperm2i128. */
#define ZIPLO8(a, b) (bs_word)__builtin_shufflevector((bs_u8v)(a), (bs_u8v)(b),  \
     0,32, 1,33, 2,34, 3,35, 4,36, 5,37, 6,38, 7,39,                            \
    16,48,17,49,18,50,19,51,20,52,21,53,22,54,23,55)
#define ZIPHI8(a, b) (bs_word)__builtin_shufflevector((bs_u8v)(a), (bs_u8v)(b),  \
     8,40, 9,41,10,42,11,43,12,44,13,45,14,46,15,47,                            \
    24,56,25,57,26,58,27,59,28,60,29,61,30,62,31,63)
#define ZIPLO16(a, b) (bs_word)__builtin_shufflevector((bs_u16v)(a), (bs_u16v)(b), \
    0,16,1,17,2,18,3,19,  8,24, 9,25,10,26,11,27)
#define ZIPHI16(a, b) (bs_word)__builtin_shufflevector((bs_u16v)(a), (bs_u16v)(b), \
    4,20,5,21,6,22,7,23, 12,28,13,29,14,30,15,31)
#define ZIPLO32(a, b) (bs_word)__builtin_shufflevector((bs_u32v)(a), (bs_u32v)(b), \
    0,8,1,9, 4,12,5,13)
#define ZIPHI32(a, b) (bs_word)__builtin_shufflevector((bs_u32v)(a), (bs_u32v)(b), \
    2,10,3,11, 6,14,7,15)
#define ZIPLO64(a, b) (bs_word)__builtin_shufflevector((bs_u64v)(a), (bs_u64v)(b), \
    0,4, 2,6)
#define ZIPHI64(a, b) (bs_word)__builtin_shufflevector((bs_u64v)(a), (bs_u64v)(b), \
    1,5, 3,7)
#define CROSSLO(a, b) (bs_word)__builtin_shufflevector((bs_u32v)(a), (bs_u32v)(b), \
    0,1,2,3,  8, 9,10,11)
#define CROSSHI(a, b) (bs_word)__builtin_shufflevector((bs_u32v)(a), (bs_u32v)(b), \
    4,5,6,7, 12,13,14,15)

/* The 8x8 byte transpose across eight rows, per u64 lane: with T_t the
 * qword whose byte i is r[i]'s byte t,
 *   out[2p]   = [lane 0: T_2p T_2p+1 | lane 2: T_2p T_2p+1]
 *   out[2p+1] = [lane 1: T_2p T_2p+1 | lane 3: T_2p T_2p+1]
 * (the interleaves work per 16-byte half, so the tree keeps lanes 0/2 in
 * one register and 1/3 in the other; the pack pays eight qword interleaves
 * to re-pair them, the unpack pays the lane crossing at the very end
 * instead).  Stages 32/16/8 over a stride-8 row set are exactly this
 * permutation: 24 shuffles in place of twelve six-operation swaps. */
static inline __attribute__((always_inline)) void
bytetr8(const bs_word r[8], bs_word out[8])
{
    bs_word a0 = ZIPLO8(r[0], r[1]), b0 = ZIPHI8(r[0], r[1]);
    bs_word a1 = ZIPLO8(r[2], r[3]), b1 = ZIPHI8(r[2], r[3]);
    bs_word a2 = ZIPLO8(r[4], r[5]), b2 = ZIPHI8(r[4], r[5]);
    bs_word a3 = ZIPLO8(r[6], r[7]), b3 = ZIPHI8(r[6], r[7]);
    bs_word c0 = ZIPLO16(a0, a1), c1 = ZIPHI16(a0, a1);
    bs_word c2 = ZIPLO16(a2, a3), c3 = ZIPHI16(a2, a3);
    bs_word d0 = ZIPLO16(b0, b1), d1 = ZIPHI16(b0, b1);
    bs_word d2 = ZIPLO16(b2, b3), d3 = ZIPHI16(b2, b3);
    out[0] = ZIPLO32(c0, c2); out[2] = ZIPHI32(c0, c2);   /* T_0 T_1, T_2 T_3 */
    out[4] = ZIPLO32(c1, c3); out[6] = ZIPHI32(c1, c3);   /* T_4 T_5, T_6 T_7 */
    out[1] = ZIPLO32(d0, d2); out[3] = ZIPHI32(d0, d2);   /* the same, lanes 1|3 */
    out[5] = ZIPLO32(d1, d3); out[7] = ZIPHI32(d1, d3);
}
#endif /* BS_LANES == 4 */

/* ---- batch gather ------------------------------------------------------- */

/* One contiguous octet of rows, in registers: lane w of row j is m[64w+j],
 * so the octet is a BS_LANES x 8 block transpose of u64s, no bit math.
 *
 * The scalar-array-plus-memcpy form below vectorizes when it runs as its
 * own pass over memory, but not when it is fused into a pass and has to
 * land in registers: gcc then builds each row element by element (vmovq,
 * vpinsrq, vinserti128).  Four lanes therefore get the block transpose
 * written out; other lane counts keep the memcpy form, which stays correct
 * whether or not the compiler vectorizes it. */
#if BS_LANES == 4
typedef uint64_t bs_u64x2 __attribute__((vector_size(16)));

static inline __attribute__((always_inline)) void
gather_oct(const uint64_t *m, int g, bs_word r[8])
{
#pragma GCC unroll 4
    for (int p = 0; p < 4; p++) {
        int i = g + 2 * p;
        bs_u64x2 q0, q1, q2, q3;
        memcpy(&q0, m + i,       16); memcpy(&q1, m + 64 + i,  16);
        memcpy(&q2, m + 128 + i, 16); memcpy(&q3, m + 192 + i, 16);
        /* each 128-bit half broadcast to both halves of a word (a plain
         * load), the pair merged by a lane blend, then one qword
         * interleave each finishes the 2x2 */
        bs_word b0 = (bs_word){q0[0], q0[1], q0[0], q0[1]};
        bs_word b1 = (bs_word){q1[0], q1[1], q1[0], q1[1]};
        bs_word b2 = (bs_word){q2[0], q2[1], q2[0], q2[1]};
        bs_word b3 = (bs_word){q3[0], q3[1], q3[0], q3[1]};
        BS_PIN(b0); BS_PIN(b1); BS_PIN(b2); BS_PIN(b3);
        bs_word a = __builtin_shufflevector(b0, b2, 0, 1, 6, 7);
        bs_word b = __builtin_shufflevector(b1, b3, 0, 1, 6, 7);
        r[2 * p]     = ZIPLO64(a, b);
        r[2 * p + 1] = ZIPHI64(a, b);
    }
}
#else
static inline __attribute__((always_inline)) void
gather_oct(const uint64_t *m, int g, bs_word r[8])
{
#pragma GCC unroll 8
    for (int i = 0; i < 8; i++) {
        int j = g + i;
#if BS_BATCH == 512
        uint64_t t[8] = { m[j],       m[64 + j],  m[128 + j], m[192 + j],
                          m[256 + j], m[320 + j], m[384 + j], m[448 + j] };
#elif BS_BATCH == 256
        uint64_t t[4] = { m[j], m[64 + j], m[128 + j], m[192 + j] };
#elif BS_BATCH == 128
        uint64_t t[2] = { m[j], m[64 + j] };
#else
        uint64_t t[1] = { m[j] };   /* one lane: the gather is a copy */
#endif
        memcpy(&r[i], t, sizeof t);   /* aliasing-safe vector store */
    }
}
#endif

/* ---- pack: masks -> planes ---------------------------------------------- */

/* Pass 2 for one stride-8 row set: rows k0+8i, i < 8, through stages
 * 32/16/8, then out to the planes.  pack_fused calls it from a rolled loop
 * over k0, so the c < 52 test runs per plane rather than folding: one copy
 * of this pass instead of eight, which would be most of the pack's text.
 *
 * At four lanes those three stages are the per-lane 8x8 byte transpose:
 * bytetr8 plus eight qword interleaves to re-pair the lanes, in place of
 * twelve masked swaps. */
static inline __attribute__((always_inline)) void
pack_pass2(const bs_word rows[64], int k0, const int *map, bs_word *planes,
           int use_map)
{
    bs_word r[8];
#if BS_LANES == 4
    bs_word in[8], o[8];
#pragma GCC unroll 8
    for (int i = 0; i < 8; i++) in[i] = rows[k0 + 8 * i];
    bytetr8(in, o);
#pragma GCC unroll 4
    for (int p = 0; p < 4; p++) {
        r[2 * p]     = ZIPLO64(o[2 * p], o[2 * p + 1]);
        r[2 * p + 1] = ZIPHI64(o[2 * p], o[2 * p + 1]);
    }
#else
#pragma GCC unroll 8
    for (int i = 0; i < 8; i++) r[i] = rows[k0 + 8 * i];
    SW(32, M32, r[0], r[4]); SW(32, M32, r[1], r[5]);
    SW(32, M32, r[2], r[6]); SW(32, M32, r[3], r[7]);
    SW(16, M16, r[0], r[2]); SW(16, M16, r[1], r[3]);
    SW(16, M16, r[4], r[6]); SW(16, M16, r[5], r[7]);
    SW(8,  M8,  r[0], r[1]); SW(8,  M8,  r[2], r[3]);
    SW(8,  M8,  r[4], r[5]); SW(8,  M8,  r[6], r[7]);
#endif
#pragma GCC unroll 8
    for (int i = 0; i < 8; i++) {
        int c = k0 + 8 * i;
        if (!use_map) planes[c] = r[i];
        else if (c < 52) planes[map[c]] = r[i];
    }
}

/* map = NULL: write all 64 planes (bs_transpose's contract); else write
 * plane c to planes[map[c]], c < 52 only.  always_inline so the use_map
 * test folds per call site. */
static inline __attribute__((always_inline)) void
pack_fused(const uint64_t masks[BS_BATCH], const int *map, bs_word *planes,
           int use_map)
{
    bs_word rows[64];
    /* Pinned rolled: unrolling copies the octet body eight times, and this
     * code shares an instruction cache with the circuit that runs between
     * the pack and the unpack, where the text costs more than the loop
     * overhead it saves.  The pragma is what pins it -- gcc unrolls the
     * loop on its own at -O3. */
#pragma GCC unroll 1
    for (int g = 0; g < 64; g += 8) {
        bs_word r[8];
        gather_oct(masks, g, r);
        LADDER421(r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
#pragma GCC unroll 8
        for (int i = 0; i < 8; i++) rows[g + i] = r[i];
    }
#pragma GCC unroll 1
    for (int k0 = 0; k0 < 8; k0++)
        pack_pass2(rows, k0, map, planes, use_map);
}

void bs_transpose(const uint64_t masks[BS_BATCH], bs_word planes[64])
{
    pack_fused(masks, NULL, planes, 0);
}

void bs_transpose_map(const uint64_t masks[BS_BATCH], const int map[52],
                      bs_word *planes)
{
    pack_fused(masks, map, planes, 1);
}

/* ---- unpack: planes -> vals --------------------------------------------- */

/* ng = ceil(nout/8), a literal at every call site, so the empty groups fold. */

#if BS_LANES == 4

/* Four lanes, byte tiles.  Per 8-plane group, stages 4/2/1 leave row l
 * byte b holding value byte g of hand 8b+l; bytetr8 turns that into
 * register b holding byte g of hands 8b..8b+7, lanes paired 0|2 and 1|3.
 * The merge assembles a u32 value out of the four byte planes with
 * widening interleaves, which is also what puts the hands in order, so
 * there is no second exchange pass and no row buffer; the 128-bit lane
 * crossing happens once per store. */
static inline __attribute__((always_inline)) void
unpack_core(const bs_word *planes, int nout, uint32_t vals[BS_BATCH], int ng)
{
    bs_word U[4][8];
    /* Pinned rolled, for the reason pack_fused's octet loop is: one group
     * body instead of four.  Nothing inside folds any more -- g against ng
     * and the plane index against nout are both tested per group, and U is
     * addressed by a variable index, so it lives on the stack rather than
     * in registers. */
#pragma GCC unroll 1
    for (int g = 0; g < 4; g++) {
        if (g < ng) {
            bs_word r[8];
#pragma GCC unroll 8
            for (int l = 0; l < 8; l++)
                r[l] = 8 * g + l < nout ? planes[8 * g + l] : ZEROW;
            LADDER421(r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
            bytetr8(r, U[g]);
        } else {
#pragma GCC unroll 8
            for (int s = 0; s < 8; s++) U[g][s] = ZEROW;
        }
    }
    /* rolled on the same terms: one store body instead of eight */
#pragma GCC unroll 1
    for (int s = 0; s < 8; s++) {
        int q = s >> 1, h = s & 1;
        /* bytes -> u16 (b0,b1) and (b2,b3) -> u32, hands 0-7 / 8-15 of 16 */
        bs_word m1 = ZIPLO8(U[0][s], U[1][s]), m2 = ZIPHI8(U[0][s], U[1][s]);
        bs_word z1 = ZIPLO8(U[2][s], U[3][s]), z2 = ZIPHI8(U[2][s], U[3][s]);
        bs_word da = ZIPLO16(m1, z1), db = ZIPHI16(m1, z1);   /* 16q+0..3, +4..7 */
        bs_word dc = ZIPLO16(m2, z2), dd = ZIPHI16(m2, z2);   /* 16q+8..11, +12..15 */
        uint32_t *pa = vals + 64 * h + 16 * q, *pb = vals + 64 * (h + 2) + 16 * q;
        bs_word o0 = CROSSLO(da, db), o1 = CROSSLO(dc, dd);
        bs_word o2 = CROSSHI(da, db), o3 = CROSSHI(dc, dd);
        memcpy(pa,     &o0, sizeof o0); memcpy(pa + 8, &o1, sizeof o1);
        memcpy(pb,     &o2, sizeof o2); memcpy(pb + 8, &o3, sizeof o3);
    }
}

#else

/* Every other lane count: the two exchange passes over 32 rows.  Pass 1 is
 * stages 4/2/1 over the plane octets that hold anything, pass 2 stages
 * 16/8 over the quads {k0, k0+8, k0+16, k0+24}, which is the smallest row
 * group closed under both stages.  Those quads still enter with zero rows,
 * hence the SW0 arms below. */
static inline __attribute__((always_inline)) void
unpack_core(const bs_word *planes, int nout, uint32_t vals[BS_BATCH], int ng)
{
    bs_word rows[32];
    /* Pinned rolled, as in pack_fused and in the four-lane spelling above:
     * one group body instead of four, at the cost of testing g against ng
     * and the plane index against nout per group rather than folding them.
     * The second pass below still folds -- its SW0 arms are selected on ng,
     * which stays a literal from bs_untranspose's switch. */
#pragma GCC unroll 1
    for (int g = 0; g < 4; g++) {
        if (g >= ng) continue;
        bs_word r[8];
#pragma GCC unroll 8
        for (int i = 0; i < 8; i++)
            r[i] = 8 * g + i < nout ? planes[8 * g + i] : ZEROW;
        LADDER421(r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
#pragma GCC unroll 8
        for (int i = 0; i < 8; i++) rows[8 * g + i] = r[i];
    }
    for (int k0 = 0; k0 < 8; k0++) {
        bs_word r0 = rows[k0];
        bs_word r1 = ng > 1 ? rows[k0 + 8]  : ZEROW;
        bs_word r2 = ng > 2 ? rows[k0 + 16] : ZEROW;
        bs_word r3 = ng > 3 ? rows[k0 + 24] : ZEROW;
        if (ng > 2) SW(16, M16, r0, r2); else SW0(16, M16, r0, r2);
        if (ng > 3) SW(16, M16, r1, r3); else if (ng > 1) SW0(16, M16, r1, r3);
        if (ng > 1) { SW(8, M8, r0, r1);  SW(8, M8, r2, r3);  }
        else        { SW0(8, M8, r0, r1); SW0(8, M8, r2, r3); }
        rows[k0] = r0; rows[k0 + 8] = r1; rows[k0 + 16] = r2; rows[k0 + 24] = r3;
    }
    /* row i carries hand i in its low half and hand 32+i in its high half */
    for (int i = 0; i < 32; i++) {
        uint64_t t[BS_LANES];
        memcpy(t, rows + i, sizeof t);
        for (int w = 0; w < BS_LANES; w++) {
            vals[64 * w + i]      = (uint32_t)t[w];
            vals[64 * w + 32 + i] = (uint32_t)(t[w] >> 32);
        }
    }
}

#endif /* BS_LANES == 4 */

void bs_untranspose(const bs_word *planes, int nout, uint32_t vals[BS_BATCH])
{
    switch ((nout + 7) >> 3) {
    case 0:
    case 1:  unpack_core(planes, nout, vals, 1); return;
    case 2:  unpack_core(planes, nout, vals, 2); return;
    case 3:  unpack_core(planes, nout, vals, 3); return;
    default: unpack_core(planes, nout, vals, 4); return;
    }
}
