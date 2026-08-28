/* Link-time-bound convenience wrappers for the workflow tools: call
 * whatever bs_eval / bs_num_outputs / bs_card_input the binary linked
 * (bitslice.py's default export names).  Kept out of lib/bspack/ so the
 * library has no weak externs -- ELF resolves the missing ones here to
 * NULL, macOS needs the Makefiles' WEAKU (-Wl,-U,...) to allow the same.
 * Binaries whose circuits are compile-time renamed use the _fn forms in
 * bsapi.h directly and do not need this file. */
#include "bsapi.h"

extern void bs_eval(const bs_word *in, bs_word *out) __attribute__((weak));
extern const int bs_num_outputs __attribute__((weak));
extern const int bs_card_input[52] __attribute__((weak));

void bs_eval_hands(const uint64_t hands[BS_BATCH], uint32_t vals[BS_BATCH])
{
    bs_eval_hands_fn(bs_eval, bs_card_input, bs_num_outputs, hands, vals);
}

void bs_eval_hands2(const uint64_t hole[BS_BATCH],
                    const uint64_t board[BS_BATCH], uint32_t vals[BS_BATCH])
{
    bs_eval_hands2_fn(bs_eval, bs_num_outputs, hole, board, vals);
}
