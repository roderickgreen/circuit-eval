// Shared helper functions for the behavioral specs.
// Verilog-2001 has no packages: `include this inside a module body.
// Bodies are written for readability -- yosys canonicalizes adder chains,
// so e.g. a linear popcount costs the same gates as a compressor tree
// (measured: both land at 47 2-input nodes for popcnt13).

function [2:0] popcnt4;
  input [3:0] x;
  integer pk;
  begin
    popcnt4 = 0;
    for (pk = 0; pk < 4; pk = pk + 1)
      popcnt4 = popcnt4 + x[pk];
  end
endfunction

function [2:0] popcnt5;
  input [4:0] x;
  integer pk;
  begin
    popcnt5 = 0;
    for (pk = 0; pk < 5; pk = pk + 1)
      popcnt5 = popcnt5 + x[pk];
  end
endfunction

function [3:0] popcnt8;
  input [7:0] x;
  integer pk;
  begin
    popcnt8 = 0;
    for (pk = 0; pk < 8; pk = pk + 1)
      popcnt8 = popcnt8 + x[pk];
  end
endfunction

function [3:0] popcnt13;
  input [12:0] x;
  integer pk;
  begin
    popcnt13 = 0;
    for (pk = 0; pk < 13; pk = pk + 1)
      popcnt13 = popcnt13 + x[pk];
  end
endfunction

// The Omaha window split predicate: a 5-rank window can supply a made
// hand iff at least 2 of its ranks can come from the hole cards and at
// least 3 from the board (the mandatory 2+3 split).  hole_w / board_w
// are per-rank "this side can supply this rank" masks for the window.
function window_split23;
  input [4:0] hole_w;
  input [4:0] board_w;
  begin
    window_split23 = (popcnt5(hole_w) >= 2) && (popcnt5(board_w) >= 3);
  end
endfunction

// The wheel view of a 13-bit rank mask: the lowest 5-rank window
// A,2,3,4,5, with the ace playing low (bit 4 = ace, bits 3:0 = 5..2).
function [4:0] wheel5;
  input [12:0] x;
  begin
    wheel5 = {x[12], x[3:0]};
  end
endfunction

// Highest straight over a 13-bit rank-presence mask: ten 5-in-a-row
// windows, the wheel (A,2,3,4,5) lowest with the ace playing low,
// scanned upward so the highest window wins.  Returns {found, high}:
// bit 4 = a straight exists, bits 3:0 = its high rank index (the
// wheel scores as 5-high, rank index 3).
function [4:0] straight_scan;
  input [12:0] m;
  reg found;
  reg [3:0] high;
  integer w;
  begin
    found = &wheel5(m);
    high = found ? 4'd3 : 4'd0;
    for (w = 1; w <= 9; w = w + 1)
      if (&m[w-1 +: 5]) begin
        found = 1'b1;
        high = w[3:0] + 4'd3;
      end
    straight_scan = {found, high};
  end
endfunction

// Highest set rank of a 13-bit pool (0 when empty; pair with |pool
// where emptiness matters).
function [3:0] top_rank13;
  input [12:0] pool;
  integer tr;
  begin
    top_rank13 = 0;
    for (tr = 0; tr < 13; tr = tr + 1)
      if (pool[tr])
        top_rank13 = tr[3:0];
  end
endfunction

// One-hot of the highest set rank of a 13-bit pool (0 when empty).
function [12:0] top_rank_bit;
  input [12:0] pool;
  begin
    top_rank_bit = |pool ? (13'd1 << top_rank13(pool)) : 13'd0;
  end
endfunction

// Top 5 set ranks of a 13-bit mask, packed nibbles {k0..k4} highest
// first, 0-filled when the mask runs out (no multiplicity: a rank
// appears at most once).
function [19:0] top5_ranks;
  input [12:0] m;
  reg [3:0] k0, k1, k2, k3, k4;
  reg [2:0] got;
  integer tk;
  begin
    got = 0; k0 = 0; k1 = 0; k2 = 0; k3 = 0; k4 = 0;
    for (tk = 12; tk >= 0; tk = tk - 1)
      if (m[tk] && got < 5) begin
        if (got == 0) k0 = tk[3:0];
        else if (got == 1) k1 = tk[3:0];
        else if (got == 2) k2 = tk[3:0];
        else if (got == 3) k3 = tk[3:0];
        else k4 = tk[3:0];
        got = got + 1;
      end
    top5_ranks = {k0, k1, k2, k3, k4};
  end
endfunction

// Per-rank strict prefix ORs: any_above(x)[r] = some bit of x is set
// strictly above rank r; any_below(x)[r] = strictly below.
function [12:0] any_above;
  input [12:0] x;
  integer sc;
  begin
    any_above[12] = 1'b0;
    for (sc = 11; sc >= 0; sc = sc - 1)
      any_above[sc] = any_above[sc+1] | x[sc+1];
  end
endfunction

function [12:0] any_below;
  input [12:0] x;
  integer sc;
  begin
    any_below[0] = 1'b0;
    for (sc = 1; sc < 13; sc = sc + 1)
      any_below[sc] = any_below[sc-1] | x[sc-1];
  end
endfunction

// Top 2 / top 3 ranks of a graded pool, descending, each rank counted
// up to its multiplicity: pN[r] = rank r can supply at least N cards
// (e.g. two cards of one rank are two kickers).  Returns packed
// nibbles {k0, k1} / {k0, k1, k2}, highest first, 0-filled when the
// pool runs out.
function [7:0] top2_mult;
  input [12:0] p1;
  input [12:0] p2;
  reg [3:0] k0, k1;
  reg [1:0] got;
  integer tk;
  begin
    got = 0; k0 = 0; k1 = 0;
    for (tk = 12; tk >= 0; tk = tk - 1) begin
      if (p1[tk] && got < 2) begin
        if (got == 0) k0 = tk[3:0];
        else k1 = tk[3:0];
        got = got + 1;
      end
      if (p2[tk] && got < 2) begin
        k1 = tk[3:0];
        got = got + 1;
      end
    end
    top2_mult = {k0, k1};
  end
endfunction

function [11:0] top3_mult;
  input [12:0] p1;
  input [12:0] p2;
  input [12:0] p3;
  reg [3:0] k0, k1, k2;
  reg [1:0] got;
  integer tk;
  begin
    got = 0; k0 = 0; k1 = 0; k2 = 0;
    for (tk = 12; tk >= 0; tk = tk - 1) begin
      if (p1[tk] && got < 3) begin
        if (got == 0) k0 = tk[3:0];
        else if (got == 1) k1 = tk[3:0];
        else k2 = tk[3:0];
        got = got + 1;
      end
      if (p2[tk] && got < 3) begin
        if (got == 1) k1 = tk[3:0];
        else k2 = tk[3:0];
        got = got + 1;
      end
      if (p3[tk] && got < 3) begin
        k2 = tk[3:0];
        got = got + 1;
      end
    end
    top3_mult = {k0, k1, k2};
  end
endfunction

function [3:0] max4;
  input [3:0] a;
  input [3:0] b;
  begin max4 = (a >= b) ? a : b; end
endfunction

function [3:0] min4;
  input [3:0] a;
  input [3:0] b;
  begin min4 = (a >= b) ? b : a; end
endfunction
