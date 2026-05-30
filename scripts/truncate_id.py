#!/usr/bin/env python3
"""
Truncate the vertex factors of a FORM `id diags<N>` substitution to a given
summand count, producing a SMALLER variant of the same script. Use for
diagnostic runs that exercise the same pipeline at reduced workload, e.g. to
get module-4 profile data when the full-scale run can't finish module 4
inside the walltime.

Algebraic note: the truncated output is NOT the original physics amplitude
(some vertex summands have been discarded). The pipeline + module structure +
splitter behavior are preserved exactly. Output is meaningless for physics
correctness; valid for performance characterization only.

Detection: any paren-wrapped depth-0 factor of the id RHS with more than
MIN_VERTEX top-level summands is treated as a "vertex" and gets truncated.
Single-summand factors (props, Etens) are passed through unchanged.

Usage: python3 truncate_id.py INPUT.frm OUTPUT.frm N_summands_per_vertex
"""

import re
import sys
from pathlib import Path

MIN_VERTEX = 10  # below this, factor is not a vertex; pass through


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
    out = []
    depth = 0
    for i, c in enumerate(inside):
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
        elif c in '+-' and depth == 0 and i > 0 and inside[i - 1] not in '*/^(,':
            out.append(i)
    return out


def truncate_factor(factor_str, N):
    """Keep only the first N summands of a paren-wrapped factor."""
    assert factor_str.startswith('(') and factor_str.endswith(')')
    inner = factor_str[1:-1]
    signs = find_top_signs(inner)
    if len(signs) + 1 <= N:
        return factor_str  # already at or below N summands
    truncated = inner[:signs[N - 1]]
    return '(' + truncated.strip() + ')'


def main(in_path, out_path, N):
    text = Path(in_path).read_text()
    m = re.search(r'id\s+(diags\d+)\s*=', text)
    if not m:
        sys.exit("ERROR: no 'id diags<N> =' found in file")
    id_name = m.group(1)
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

    new_factors = []
    truncated_idx = []
    for i, f in enumerate(factors):
        if f.startswith('(') and f.endswith(')'):
            inner = f[1:-1]
            n_summands = len(find_top_signs(inner)) + 1
            if n_summands >= MIN_VERTEX and n_summands > N:
                tf = truncate_factor(f, N)
                new_factors.append(tf)
                truncated_idx.append((i, n_summands, N, len(f), len(tf)))
                continue
        new_factors.append(f)

    new_rhs = '*'.join(new_factors)
    output_text = text[:rhs_start] + new_rhs + text[rhs_end:]
    Path(out_path).write_text(output_text)

    print(f'id symbol: {id_name}')
    print(f'truncated {len(truncated_idx)} vertex factors to N={N} summands each:')
    for i, n_old, n_new, len_old, len_new in truncated_idx:
        print(f'  factor[{i}]: {n_old} -> {n_new} summands ({len_old} -> {len_new} chars)')
    print(f'wrote {out_path}: {len(output_text):,} bytes (input {len(text):,})')


if __name__ == '__main__':
    if len(sys.argv) != 4:
        sys.exit('usage: truncate_id.py INPUT.frm OUTPUT.frm N_summands_per_vertex')
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]))
