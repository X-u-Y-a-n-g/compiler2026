# Paired-K conditional matrix reduction: rejected

Date: 2026-07-31

Candidate `6dd8989` paired two adjacent K rows in the existing AArch64
conditional matrix-reduction kernel. It retained increasing K order and
sequentially streamed both source rows while halving output loads and stores.
The transformation used only the existing general matrix/alias/parity proof
and did not inspect benchmark names, paths, source fingerprints, or inputs.

The official Cortex-A53 run passed all 60 cases, but `matmul1/2/3` changed from
the accepted parent result `4.68/4.69/4.69s` to `5.58/5.59/5.56s`. Their total
increased from `14.06s` to `16.73s`, a `19.0%` regression. The modest reduction
in output traffic did not compensate for the second active source stream,
extra parity dispatch, larger loop body, and additional front-end pressure on
Cortex-A53. The unrelated `shuffle1` movement from `14.97s` to `14.85s` is not
attributable evidence for this matrix-only transformation.

Status: rejected and isolated-reverted immediately. Do not restore adjacent-K
pairing or the previously rejected J-panel/full-K interchange. Future matrix
candidates must retain the accepted single-row streaming kernel or remove
larger algorithmic work outside that hot kernel.

Docker was unavailable, so the repository's no-Docker publication exception
was used. AI assistance covered implementation, semantic review, official
result analysis, and rollback documentation. No external implementation was
copied.
