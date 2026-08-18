#!/usr/bin/env python3
"""
Generalized vertex-splitter for any FORM `id diags<N> = <big product>;`.

- Auto-detects the id symbol (any `id diags<digits>`).
- Auto-detects "vertex" factors (paren-wrapped depth-0 factors whose inner
  content has >= MIN_SUMMANDS top-level summands).
- Distributes K_target across the V vertex factors with a uniform per-vertex
  N = round(K_target ** (1/V)), capped at each vertex's summand count.
- Flips the nearest preceding `off parallel;` to `on parallel;` so the
  K subsequent id-substitutions run parallel after the new `.sort`.
- Algebraic equivalence is by construction: opaque symbols substituted back
  rewrite (V1)*(V2)*... into itself. Validate with byte-identical `h_h_h*.out`
  against the unsplit baseline.

Usage:
    python3 split_id.py INPUT.frm OUTPUT.frm K_target
"""
import re
import sys
from pathlib import Path

MIN_SUMMANDS_FOR_VERTEX = 10  # factors with fewer summands don't get split


def split_top_star(s):
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


def find_top_signs(inside):
    splits = []
    depth = 0
    for i, c in enumerate(inside):
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
        elif c in '+-' and depth == 0 and i > 0:
            if inside[i - 1] in '*/^(,':
                continue
            splits.append(i)
    return splits


def chunk_factor(inner, N):
    splits = find_top_signs(inner)
    summands = []
    prev = 0
    for pos in splits:
        summands.append(inner[prev:pos])
        prev = pos
    summands.append(inner[prev:])

    n = len(summands)
    if n < N:
        raise ValueError(f"need {N} chunks but only {n} summands")
    base, rem = divmod(n, N)
    chunks = []
    idx = 0
    for k in range(N):
        size = base + (1 if k < rem else 0)
        s = ''.join(summands[idx:idx + size]).strip()
        if s.startswith('+'):
            s = s[1:].lstrip()
        chunks.append(s)
        idx += size
    return chunks


def make_split(input_path, output_path, K_target):
    text = Path(input_path).read_text()

    # locate id <name> = ... ;
    m = re.search(r'id\s+(diags\d+)\s*=', text)
    if not m:
        sys.exit("ERROR: no 'id diags<N> =' found in file")
    id_name = m.group(1)
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
        sys.exit("ERROR: id terminator ';' not found")

    rhs = text[rhs_start:rhs_end].strip()
    factors = split_top_star(rhs)

    # find vertex factors (those with >= MIN_SUMMANDS_FOR_VERTEX summands)
    vertex_indices, vertex_summand_counts = [], []
    for i, f in enumerate(factors):
        if not (f.startswith('(') and f.endswith(')')):
            continue
        inside = f[1:-1]
        n = len(find_top_signs(inside)) + 1
        if n >= MIN_SUMMANDS_FOR_VERTEX:
            vertex_indices.append(i)
            vertex_summand_counts.append(n)

    V = len(vertex_indices)
    if V == 0:
        sys.exit(f"ERROR: no factors with >= {MIN_SUMMANDS_FOR_VERTEX} summands")

    # uniform N per vertex = round(K^(1/V)); cap at each vertex's summands
    N_uniform = max(1, round(K_target ** (1.0 / V)))
    Ns = [min(N_uniform, vertex_summand_counts[i]) for i in range(V)]
    K_actual = 1
    for n in Ns:
        K_actual *= n

    print(f'id symbol: {id_name}')
    print(f'factors: {len(factors)} total; '
          f'{V} are vertex (idx {vertex_indices}, summands {vertex_summand_counts})')
    print(f'K_target = {K_target} -> uniform N = {N_uniform} per vertex '
          f'-> per-vertex Ns = {Ns} -> K_actual = {K_actual}')

    # chunk each vertex
    all_chunks, all_syms_flat = {}, []
    new_factors = list(factors)
    for vi, idx in enumerate(vertex_indices):
        N = Ns[vi]
        chunks = chunk_factor(factors[idx][1:-1], N)
        syms = [f'dgs{id_name[5:]}V{idx}c{c}' for c in range(N)]  # e.g. dgs9V7c0, dgs45V13c5
        all_chunks[idx] = (syms, chunks)
        all_syms_flat.extend(syms)
        new_factors[idx] = '(' + '+'.join(syms) + ')'

    new_rhs = '*'.join(new_factors)

    lines = []
    parts = [f'F[{idx}]={Ns[vi]}({vertex_summand_counts[vi]}sum)'
             for vi, idx in enumerate(vertex_indices)]
    lines.append(f'* === {id_name} SPLIT into K={K_actual} chunks: '
                 + ' x '.join(parts) + ' ===\n')
    lines.append(f'S {",".join(all_syms_flat)};\n')
    lines.append(f'id {id_name} = {new_rhs};\n')
    lines.append('.sort\n')
    for idx in vertex_indices:
        syms, chunks = all_chunks[idx]
        for s, c in zip(syms, chunks):
            lines.append(f'id {s} = {c};\n')
    lines.append(f'* === end split block (K={K_actual}) ===\n')
    replacement = ''.join(lines)

    # flip nearest preceding `off parallel;` to `on parallel;`
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

    output_text = text_before + replacement + text[rhs_end + 1:]
    Path(output_path).write_text(output_text)

    print(f'Wrote {output_path}: K={K_actual}, {len(all_syms_flat)} symbols, '
          f'{len(output_text):,} bytes (input {len(text):,})')


if __name__ == '__main__':
    if len(sys.argv) != 4:
        sys.exit('usage: split_id.py INPUT.frm OUTPUT.frm K_target')
    make_split(sys.argv[1], sys.argv[2], int(sys.argv[3]))
