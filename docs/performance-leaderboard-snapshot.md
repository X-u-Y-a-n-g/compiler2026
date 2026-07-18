# 2026 Performance Leaderboard Snapshot

This file is the persistent performance reference extracted from the complete
leaderboard screenshot captured on 2026-07-14 23:09 CST. The screenshot was
described as the latest `wz` result at that time. Times are seconds on the
official FPGA evaluation environment.

The leaderboard changes over time. Treat this as a dated optimization baseline,
not a permanent claim about the current best result. Values displayed as `0.00`
are below the page's two-decimal display precision; they do not mean zero target
cycles and cannot be used to infer a special-case transformation.

## Compliance gate

Every optimization guided by this table must remain general and compliant with
the contest rules in `AGENTS.md`:

- do not match function names, global names, exact basic-block names, fixed
  benchmark dimensions, input patterns, known outputs, or other test identity;
- do not replace a recognized benchmark with a different hard-coded algorithm;
- require a general legality analysis and preserve observable SysY semantics;
- use the table only to select which general compiler weakness to investigate;
- accept performance changes based on real FPGA target cycles, then verify
correctness before committing and pushing to `wz`.

## Snapshot

`Gap` is `snapshot time / best time`, rounded to two decimals. It is omitted
when the displayed best time is `0.00`.

| # | Test | Snapshot time | Best time | Gap |
|---:|---|---:|---:|---:|
| 1 | `01_mm1` | 1.99 | 1.33 | 1.50x |
| 2 | `01_mm2` | 6.80 | 4.67 | 1.46x |
| 3 | `01_mm3` | 4.36 | 2.93 | 1.49x |
| 4 | `03_sort1` | 1.96 | 0.53 | 3.70x |
| 5 | `03_sort2` | 1.97 | 0.53 | 3.72x |
| 6 | `03_sort3` | 1.96 | 0.53 | 3.70x |
| 7 | `conv2d-1` | 107.67 | 0.88 | 122.35x |
| 8 | `conv2d-2` | 29.38 | 0.24 | 122.42x |
| 9 | `conv2d-3` | 9.67 | 0.08 | 120.88x |
| 10 | `crc1` | 4.20 | 0.00 | - |
| 11 | `crc2` | 4.21 | 0.00 | - |
| 12 | `crc3` | 4.21 | 0.00 | - |
| 13 | `crypto-1` | 5.89 | 2.81 | 2.10x |
| 14 | `crypto-2` | 4.12 | 1.97 | 2.09x |
| 15 | `crypto-3` | 2.36 | 1.13 | 2.09x |
| 16 | `fft0` | 0.21 | 0.02 | 10.50x |
| 17 | `fft1` | 2.17 | 0.15 | 14.47x |
| 18 | `fft2` | 0.97 | 0.07 | 13.86x |
| 19 | `h-1-01` | 0.79 | 0.15 | 5.27x |
| 20 | `h-1-02` | 0.06 | 0.01 | 6.00x |
| 21 | `h-1-03` | 4.55 | 0.85 | 5.35x |
| 22 | `h-10-01` | 0.49 | 0.05 | 9.80x |
| 23 | `h-10-02` | 1.17 | 0.12 | 9.75x |
| 24 | `h-10-03` | 2.26 | 0.23 | 9.83x |
| 25 | `h-4-01` | 1.39 | 0.44 | 3.16x |
| 26 | `h-4-02` | 0.00 | 0.00 | - |
| 27 | `h-4-03` | 18.99 | 6.26 | 3.03x |
| 28 | `h-5-01` | 0.00 | 0.00 | - |
| 29 | `h-5-02` | 0.00 | 0.00 | - |
| 30 | `h-5-03` | 0.00 | 0.00 | - |
| 31 | `h-8-01` | 0.01 | 0.00 | - |
| 32 | `h-8-02` | 0.01 | 0.00 | - |
| 33 | `h-8-03` | 0.01 | 0.00 | - |
| 34 | `h-9-01` | 2.12 | 2.12 | 1.00x |
| 35 | `h-9-02` | 1.41 | 1.41 | 1.00x |
| 36 | `h-9-03` | 1.76 | 1.76 | 1.00x |
| 37 | `huffman-01` | 7.30 | 0.21 | 34.76x |
| 38 | `huffman-02` | 7.36 | 0.21 | 35.05x |
| 39 | `huffman-03` | 7.31 | 0.21 | 34.81x |
| 40 | `knapsack_naive-1` | 31.48 | 0.00 | - |
| 41 | `knapsack_naive-2` | 31.46 | 0.00 | - |
| 42 | `knapsack_naive-3` | 31.46 | 0.00 | - |
| 43 | `many_mat_cal-1` | 106.16 | 8.38 | 12.67x |
| 44 | `many_mat_cal-2` | 105.45 | 8.32 | 12.67x |
| 45 | `many_mat_cal-3` | 104.51 | 8.26 | 12.65x |
| 46 | `matmul1` | 5.71 | 1.49 | 3.83x |
| 47 | `matmul2` | 23.99 | 5.91 | 4.06x |
| 48 | `matmul3` | 12.61 | 2.80 | 4.50x |
| 49 | `optimization_scheduling1` | 7.16 | 0.00 | - |
| 50 | `optimization_scheduling2` | 2.20 | 0.00 | - |
| 51 | `optimization_scheduling3` | 0.00 | 0.00 | - |
| 52 | `shuffle0` | 0.75 | 0.55 | 1.36x |
| 53 | `shuffle1` | 4.49 | 3.38 | 1.33x |
| 54 | `shuffle2` | 0.64 | 0.43 | 1.49x |
| 55 | `sl1` | 7.66 | 4.04 | 1.90x |
| 56 | `sl2` | 15.28 | 7.98 | 1.91x |
| 57 | `sl3` | 0.86 | 0.48 | 1.79x |
| 58 | `transpose0` | 0.61 | 0.33 | 1.85x |
| 59 | `transpose1` | 1.10 | 0.00 | - |
| 60 | `transpose2` | 14.69 | 0.01 | 1469.00x |

## Current hotspot order

Use absolute time and gap together. The first structural investigations are:

1. `many_mat_cal`: about 105 seconds each and roughly 12.7x behind best;
2. `conv2d`: up to 108 seconds and roughly 121-122x behind best;
3. `huffman`: about 7.3 seconds and roughly 35x behind best;
4. `transpose2`: 14.69 seconds, with the displayed best below 0.02 seconds;
5. `fft` and `h-10`: consistent 10-14x family-wide gaps.

Family-wide gaps are evidence of missing general transformations, not permission
to recognize those families. Investigate loop structure, memory access,
redundant address computation, scalar promotion, induction variables, calling
conventions, register pressure, instruction selection, and target scheduling.
