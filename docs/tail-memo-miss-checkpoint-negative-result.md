# Tail memo miss checkpoint: rejected

Date: 2026-07-31

Candidate `b90c06f` augmented the proven single-context dense additive-tail
memo with one additional summary for the last in-window empty state visited by
each traversal. It preserved the recurrence, cache key, cache size, empty
encoding, and fallback behavior, and did not affect the general helper memo.

All 60 official Cortex-A53 cases passed, but `h-1-01/02/03` changed from the
parent result `11.91/1.46/6.55s` to `13.18/2.19/7.99s`. The total increased
from `19.92s` to `23.36s`, a `17.3%` total-time regression. The extra moves on
every empty lookup and second cache store outweighed the gradually increased
state coverage, especially on the short second case.

Status: rejected and isolated-reverted immediately. Do not restore per-miss
checkpoint tracking, packed helper-cache entries, or extra conflict probes.
Future memoization candidates must remove repeated state-transition work
rather than add work to every miss.

Docker was unavailable, so the repository's no-Docker publication exception
was used. AI assistance covered the implementation, semantic audit, official
result analysis, and rollback record. No external implementation was copied
and no benchmark identity was used as a transformation trigger.
