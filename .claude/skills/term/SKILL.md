---
name: term
description: Reference for FORM term memory layout and processing. Use when working with term data structures in the FORM/ParFORM/MRmpi codebase — reading terms, iterating subterms, extracting coefficients, delta compression, or writing code that routes/hashes terms.
---

A FORM term is a flat `WORD` (`int32_t`) array. Everything needed to read one is in `sources/declare.h`.

## Memory Layout

```
[ total_len | subterm_1 | subterm_2 | ... | coefficient... | signed_coeff_size ]
  term[0]    term[1]...                                      term[total_len-1]
```

- `term[0]` — total length of the term in words, **inclusive**
- `term[1]` through the coefficient boundary — sequence of subterms
- final words — the rational coefficient (variable size)

## Key Macros (sources/declare.h)

```c
#define ABS(x)          ((x) < 0 ? -(x) : (x))
#define REDLENG(x)      ((((x)<0)?((x)+1):((x)-1))/2)
#define GETSTOP(x, y)   y = x+(*x)-1;  y -= ABS(*y)-1
#define GETCOEF(x, y)   x += *x;  y = x[-1];  x -= ABS(y);  y = REDLENG(y)
#define NEXTARG(x)      if(*x>0) x+=*x; else if(*x<=-FUNCTION) x++; else x+=2
```

## Finding the Coefficient Boundary

```c
WORD *stopper;
GETSTOP(term, stopper);
// subterms: [term+1 .. stopper)
// coefficient: [stopper .. term+term[0])
```

How `GETSTOP` works:
1. `y = term + term[0] - 1` → last word of the term
2. Last word holds the **signed coefficient word-count**: `±N`
3. `y -= ABS(y) - 1` → backs up to the first coefficient word

## Extracting the Coefficient

```c
WORD *tmp = term;
WORD ncoeff;
GETCOEF(tmp, ncoeff);
// tmp now points at coefficient data
// UWORD *coeff = (UWORD *)tmp
// ABS(ncoeff) = number of UWORD pairs
// sign of original last word = sign of the coefficient
```

The coefficient is stored as **numerator** followed by **denominator**, each `ABS(ncoeff)` `UWORD`s long.

`REDLENG` converts the encoded last word to the actual pair count:
- positive N → `(N-1)/2` pairs
- negative N → `(N+1)/2` pairs

## Iterating Subterms

```c
WORD *t = term + 1;
WORD *stopper;
GETSTOP(term, stopper);

while (t < stopper) {
    WORD type = t[0];   // subterm type code
    WORD size = t[1];   // total size of this subterm in words
    // inspect t[2 .. size-1]
    t += size;          // advance to next subterm
}
```

## Subterm Type Codes (sources/ftypes.h)

| Constant | Value | Layout |
|----------|-------|--------|
| `SYMBOL` | 1 | `[1, size, sym_id, power, ...]` — pairs of (symbol id, power) |
| `DOTPRODUCT` | 2 | `[2, size, v1, v2, power, ...]` |
| `VECTOR` | 3 | `[3, size, vec, idx, ...]` |
| `INDEX` | 4 | `[4, size, idx, ...]` |
| `SNUMBER` | 16 | short integer literal |
| `LNUMBER` | 17 | long integer literal |
| `HAAKJE` | 18 | bracket marker |
| `FUNCTION+N` | 20+N | function subterm (see below) |

## Function Subterm Layout

```
[ func_id | total_size | flags | arg1 | arg2 | ... ]
  t[0]      t[1]         t[2]   t[FUNHEAD=3]...
```

`FUNHEAD = 3` — function header is always 3 words. Arguments begin at `t + FUNHEAD`.

Each argument is one of:

```c
// sub-expression (positive length word):
[ arg_len | dirty_flag | term1 | term2 | ... ]
  a[0]      a[1]         a[ARGHEAD=2]...

// built-in function reference (value <= -FUNCTION): 1 word
[ -func_id ]

// symbol/index pair: 2 words
[ element_id | exponent ]
```

Advance with `NEXTARG(ptr)`.

## Complete Term Walk

```c
void walk_term(WORD *term) {
    WORD *t, *stopper;
    GETSTOP(term, stopper);       // find coefficient boundary

    t = term + 1;
    while (t < stopper) {
        WORD type = t[0];
        WORD size = t[1];

        if (type == SYMBOL) {
            WORD *s = t + 2;
            while (s < t + size) {
                WORD sym_id  = s[0];
                WORD sym_pow = s[1];
                s += 2;
            }
        }
        else if (type >= FUNCTION) {
            WORD *arg = t + FUNHEAD;   // t + 3
            WORD *fend = t + size;
            while (arg < fend) {
                NEXTARG(arg);
            }
        }
        t += size;
    }

    // coefficient
    WORD *tmp = term;
    WORD ncoeff;
    GETCOEF(tmp, ncoeff);
    UWORD *num = (UWORD *)tmp;
    UWORD *den = num + ABS(ncoeff);
    int negative = (term[term[0]-1] < 0);
}
```

## Delta Compression (scratch file wire format)

Terms written to scratch files via `PutOut()` ([sort.c:1642](sources/sort.c#L1642)) can be delta-compressed against the **previous term written to the same stream**. Active when `ncomp > 0 && !AR.NoCompress && AR.sLevel <= 0`.

### Compress buffer

`AR.CompressBuffer` / `AR.CompressPointer` holds the full previous term (updated after every `PutOut` call). In MR mapper mode each reducer destination has its own: `AR.CompressBuffers[dst]` / `AR.CompressPointers[dst]`.

### How compression works (PutOut, sort.c:1786)

1. `r = AR.CompressPointer` — points to the previous term (length word first)
2. Walk `j = *r++ - 1` (previous length−1) and `i = *term - 1` (current length−1) in parallel, comparing word-by-word up to but **not past the coefficient** (`p < sa` where `sa` = coefficient boundary via `GETSTOP`). Counter `k` starts at 0 and decrements for each matching word.
3. Decision:
   - `k > -2` (fewer than 2 shared words): **no compression** — copy full term to compress buffer, write normally.
   - `k ≤ -2`: **compressed** — write a 2-word header followed by the unmatched tail:

```
on-disk compressed term:
  [ k | j | tail_words... ]
    ^   ^
    |   remaining word count (from the first differing word to end, including coefficient)
    negative: -(number of matched prefix words, not counting the length word itself)
```

The compress buffer is then updated to hold the full reconstructed term for the next call. The first coefficient word in the compress buffer is zeroed out afterwards (`r[-(ABS(r[-1]))] = 0`) to prevent spurious future matches.

### Reading back (GetTerm, store.c:992)

```c
len = i = *inp;   // first word from file
if ( i < 0 ) {   // compressed
    start = term;
    *term++ = -i + 1;                  // placeholder: prefix count + 1
    while ( ++i <= 0 ) *term++ = *r++; // copy -k words from AR.CompressBuffer
    i = *inp++;                        // read j: remaining word count
    *start += i;                       // fix total length = (prefix+1) + remaining
    *AR.CompressBuffer = *start;       // update compress buffer length word
    // then read i more words from file into term
}
```

After reconstruction `AR.CompressBuffer` holds the full term, ready for the next compressed read.

### ncomp values
| `ncomp` | Meaning |
|---------|---------|
| `> 0` | Compress and update compress buffer |
| `< 0` | Update compress buffer but force uncompressed output (used for deferred brackets) |
| `0` | No compression, no compress-buffer update (prototype / prototype header) |

### Not compressed
- `AT.SS != AT.S0` (nested sort level > 0, e.g. function arguments) — `PutOut` writes raw terms, no compression.
- `AR.NoCompress` flag set.
- The term itself is a prototype header (`*term == 0`).

## Relevance to MRmpi

In `sort.c` `PF_LowMRsort`, the hash of `[term+1, stopper)` (subterms only, coefficient excluded) determines which reducer receives the term. Terms with equal symbolic parts but different coefficients land on the same reducer so they are summed there — eliminating duplicates before they reach the master.
