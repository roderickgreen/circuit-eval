/* Implementation of the circuit_eval.h calls.  Each one is the same three
 * steps: transpose the batch's presence masks into bit planes (bspack.c), run
 * the raw circuit evaluator on them (the generated circuit_*.c files), and
 * transpose the output planes back into one value per hand. */
#include "circuit_eval.h"

void circuit_eval_holdem(const uint64_t hands[BS_BATCH], uint32_t vals[BS_BATCH])
{
    bs_eval_hands_fn(circuit_eval_holdem_raw, circuit_eval_holdem_raw_card_input,
                     circuit_eval_holdem_raw_num_outputs, hands, vals);
}

void circuit_eval_omaha(const uint64_t hole[BS_BATCH], const uint64_t board[BS_BATCH],
                        uint32_t vals[BS_BATCH])
{
    bs_eval_hands2_fn(circuit_eval_omaha_raw,
                      circuit_eval_omaha_raw_num_outputs, hole, board, vals);
}

void circuit_eval_omaha_hilo(const uint64_t hole[BS_BATCH],
                             const uint64_t board[BS_BATCH],
                             uint32_t high[BS_BATCH], uint32_t low[BS_BATCH])
{
    /* The high and low circuits read the same 104 input planes (hole card
     * c at [c], board card c at [52 + c]), so the input is packed once and
     * evaluated twice.  Each bs_transpose writes a full 64 planes: the
     * hole's planes 52..63 are overwritten by the board's, and the board's
     * planes 104..115 are unused -- hence in[116]. */
    bs_word in[116], outh[24], outl[8];
    bs_transpose(hole, in);
    bs_transpose(board, in + 52);
    circuit_eval_omaha_raw(in, outh);
    circuit_eval_omaha_low_raw(in, outl);
    bs_untranspose(outh, circuit_eval_omaha_raw_num_outputs, high);
    bs_untranspose(outl, circuit_eval_omaha_low_raw_num_outputs, low);
}
