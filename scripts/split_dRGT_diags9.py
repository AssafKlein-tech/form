#!/usr/bin/env python3
"""
Split the giant `id diags9 = ...;` in dRGT_h3_9_mr.frm into a parallel-
friendly form by chunking the three vertex factors (factors 7, 8, 9 of the
top-level product), each of which is a sum of 399 summands at depth 0.

3-D chunking strategy
---------------------
Original:
    off parallel;
    id diags9 = (P0)*(Etens..)*(Etens..)*(Etens..)*(prop4)*(prop5)*(prop6)
                * (V7)*(V8)*(V9);
    .sort:Diagram Loaded;

Split (with N7,N8,N9 chunks per vertex):
    on parallel;
    S diag9V7c0,...,diag9V7c{N7-1}, diag9V8c0,..., diag9V9c{N9-1};
    id diags9 = (P0)*(Etens..)*(Etens..)*(Etens..)*(prop4)*(prop5)*(prop6)
                * (diag9V7c0+...+diag9V7c{N7-1})
                * (diag9V8c0+...+diag9V8c{N8-1})
                * (diag9V9c0+...+diag9V9c{N9-1});
    .sort                       <- expr1 now has N7*N8*N9 terms distributed
    id diag9V7c0 = <V7 chunk 0>; ... id diag9V7c{N7-1} = <V7 chunk N7-1>;
    id diag9V8c0 = <V8 chunk 0>; ... id diag9V8c{N8-1} = <V8 chunk N8-1>;
    id diag9V9c0 = <V9 chunk 0>; ... id diag9V9c{N9-1} = <V9 chunk N9-1>;
    .sort:Diagram Loaded;

Why 3-D: after the first id, the product distribution is
(everything-else) * N7 * N8 * N9 terms -- vs original
(everything-else) * 399 * 399 * 399. With N=4 the first-id work is
4^3 / 399^3 ~= 1e-6 of the original -- effectively free. The subsequent
chunk-substitution work is K = N7*N8*N9 way parallel across mappers.

Algebraic equivalence: introducing opaque symbols + substituting them back is
a pure rewrite of (V7)*(V8)*(V9) into itself. FORM treats the new symbols as
scalars; substituting them with their chunks recovers the original sum
position-by-position. Verified by structure, not yet runtime.

Usage: python3 split_dRGT_diags9.py <input.frm> <output.frm> N7 N8 N9
"""
import re
import sys
from pathlib import Path


def split_top_star(s):
    """Split a string at top-level (depth-0) `*` characters."""
    out, depth, start = [], 0, 0
    for i, c in enumerate(s):
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
        elif c == '*' and depth == 0:
            out.append(s[start:i])
            start = i + 1
    out.append(s[start:])
    return [x.strip() for x in out if x.strip()]


def find_top_level_signs(inside):
    """Find positions of `+` or `-` at depth 0 of inside. Skip unary
    signs (those following an operator or open-paren or comma)."""
    splits = []
    depth = 0
    for i, c in enumerate(inside):
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
        elif c in '+-' and depth == 0 and i > 0:
            prev = inside[i - 1]
            if prev in '*/^(,':
                continue
            splits.append(i)
    return splits


def split_factor_into_chunks(factor_inner, N):
    """Split factor inner content (after stripping outer parens) into N chunks
    at top-level `+`/`-` boundaries. Returns list of N expression strings."""
    sign_positions = find_top_level_signs(factor_inner)
    summands = []
    prev = 0
    for pos in sign_positions:
        summands.append(factor_inner[prev:pos])
        prev = pos
    summands.append(factor_inner[prev:])

    n = len(summands)
    if n < N:
        raise ValueError(f"factor has {n} summands but N={N} chunks requested")

    chunks = []
    base, rem = n // N, n % N
    idx = 0
    for k in range(N):
        size = base + (1 if k < rem else 0)
        s = ''.join(summands[idx:idx + size]).strip()
        # strip leading + (the first summand may have implicit + when concatenated)
        if s.startswith('+'):
            s = s[1:].lstrip()
        chunks.append(s)
        idx += size
    return chunks


def make_split(input_path, output_path, N7, N8, N9):
    text = Path(input_path).read_text()

    # locate `id diags9 = ... ;` (the giant one)
    m = re.search(r'id\s+diags9\s*=', text)
    if not m:
        sys.exit("ERROR: 'id diags9 =' not found")
    id_start = m.start()
    rhs_start = m.end()
    depth = 0
    rhs_end = None
    for i in range(rhs_start, len(text)):
        c = text[i]
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
        elif c == ';' and depth == 0:
            rhs_end = i
            break
    if rhs_end is None:
        sys.exit("ERROR: id terminator ';' not found at depth 0")

    rhs = text[rhs_start:rhs_end].strip()
    factors = split_top_star(rhs)
    if len(factors) != 10:
        sys.exit(f"ERROR: expected 10 factors at depth 0, got {len(factors)}")
    for i in (7, 8, 9):
        if not (factors[i].startswith('(') and factors[i].endswith(')')):
            sys.exit(f"ERROR: factor {i} is not paren-wrapped")

    # chunk each vertex
    chunks_7 = split_factor_into_chunks(factors[7][1:-1], N7)
    chunks_8 = split_factor_into_chunks(factors[8][1:-1], N8)
    chunks_9 = split_factor_into_chunks(factors[9][1:-1], N9)

    syms_7 = [f'diag9V7c{i}' for i in range(N7)]
    syms_8 = [f'diag9V8c{i}' for i in range(N8)]
    syms_9 = [f'diag9V9c{i}' for i in range(N9)]
    all_syms = syms_7 + syms_8 + syms_9

    new_factors = list(factors)
    new_factors[7] = '(' + '+'.join(syms_7) + ')'
    new_factors[8] = '(' + '+'.join(syms_8) + ')'
    new_factors[9] = '(' + '+'.join(syms_9) + ')'
    new_rhs = '*'.join(new_factors)

    # Build the replacement block
    K = N7 * N8 * N9
    lines = []
    lines.append(f'* === diags9 SPLIT: V7={N7} x V8={N8} x V9={N9} = {K} parallel chunks ===\n')
    lines.append(f'S {",".join(all_syms)};\n')
    lines.append(f'id diags9 = {new_rhs};\n')
    lines.append('.sort\n')
    for sym, chunk in zip(syms_7, chunks_7):
        lines.append(f'id {sym} = {chunk};\n')
    for sym, chunk in zip(syms_8, chunks_8):
        lines.append(f'id {sym} = {chunk};\n')
    for sym, chunk in zip(syms_9, chunks_9):
        lines.append(f'id {sym} = {chunk};\n')
    lines.append(f'* === end split block ({K} chunks) ===\n')
    replacement = ''.join(lines)

    # Flip the nearest preceding `off parallel;` to `on parallel;` (so the chunk
    # id-statements actually run in parallel after the first .sort).
    text_before = text[:id_start]
    off_re = re.compile(r'(^|\n)([ \t]*)off\s+parallel\s*;([ \t]*)(\n|$)')
    last_match = None
    for mo in off_re.finditer(text_before):
        last_match = mo
    if last_match:
        text_before = (text_before[:last_match.start()]
                       + last_match.group(1)
                       + last_match.group(2) + 'on parallel;'
                       + last_match.group(3)
                       + last_match.group(4)
                       + text_before[last_match.end():])

    text_after = text[rhs_end + 1:]
    output_text = text_before + replacement + text_after

    Path(output_path).write_text(output_text)

    print(f'Wrote {output_path}')
    print(f'  V7 chunks: {N7} (summands per chunk ~ {[len(c) for c in chunks_7]})')
    print(f'  V8 chunks: {N8}')
    print(f'  V9 chunks: {N9}')
    print(f'  total parallel chunks K = {K}')
    print(f'  new symbols: {len(all_syms)} ({all_syms[0]}..{all_syms[-1]})')
    print(f'  output file size: {len(output_text):,} bytes (input was {len(text):,})')


if __name__ == '__main__':
    if len(sys.argv) != 6:
        sys.exit('usage: split_dRGT_diags9.py INPUT.frm OUTPUT.frm N7 N8 N9')
    make_split(sys.argv[1], sys.argv[2],
               int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]))
