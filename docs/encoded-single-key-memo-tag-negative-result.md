# Encoded single-key memo tag: rejected

Date: 2026-07-31

Candidate `b4bd919` replaced separate i32 valid/key arrays for a proven
phase-local single-key read-only-helper cache with a bijective i64 `key+1`
tag. Capacity, direct mapping, full-key validation, overwrite policy, value
layout, and fallback semantics were unchanged.

All 60 official Cortex-A53 cases passed. Relative to its direct parent,
`shuffle1` regressed from `14.85s` to `15.94s` (`+7.3%`); `shuffle0/2`
remained `7.59/0.15s`. The reduced branch count did not offset wider metadata
loads and tag materialization. There is no target improvement supporting
retention.

Status: rejected and isolated-reverted immediately. Together with the earlier
packed-record and second-probe results, this closes further cache-layout and
collision-probe tuning for this helper. Future work must reduce the underlying
state-transition or chain-traversal count.

Docker was unavailable, so the repository's no-Docker publication exception
was used. AI assistance covered implementation, proof, official-result
analysis, and rollback documentation. No external implementation was copied
and no benchmark identity was used as a transformation trigger.
