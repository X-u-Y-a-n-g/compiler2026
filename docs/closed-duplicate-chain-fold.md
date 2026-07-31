# Closed duplicate-chain aggregation candidate

## General transformation

This candidate recognizes an append-only integer side chain built with a fresh
monotonic node index.  A duplicate append copies the old link to the fresh node,
links the primary node to that fresh node, and stores the appended i32 payload.
The transformation replaces only the primary-link store with an i32 addition
into the primary payload.  Allocation, the fresh-node payload store, the copied
link store, return values, and all other original effects remain unchanged.

The sole permitted reader must be an exact zero-terminated SSA reduction with
the recurrences `node = link[node]` and `sum = sum + payload[node]`.  Both arrays
must be closed to the builder and that unique reader, the link array must have a
zero initializer, every other link write must be either zero initialization or
the matched old-link copy, and the append index must be `counter + 1` committed
back to the same scalar counter.  Ambiguous candidates, aliases, escaping
addresses, extra readers/writers, multiple reductions, or non-additive uses
fall back unchanged.

The invariant is that the primary payload is the modular i32 sum of the
original primary value and every duplicate payload.  Since the transformed
primary link remains zero, the unchanged reader executes one iteration and
returns the same modular sum.  The optimization therefore changes duplicate
construction and reduction from linear chain work to constant work while
retaining the original allocation side effects.

The opt-out switch is
`COMPILER_DISABLE_CLOSED_DUPLICATE_CHAIN_FOLD=1`.

## Acceptance notes

The required anonymous validation matrix covers no duplicates, one and many
duplicates, hash collisions with distinct primary nodes, negative payloads,
i32 wraparound, absent keys, zero-trip build/query phases, and rejection when
either array escapes or has an additional observer.  Docker is installed but
its daemon is unavailable in this workspace, so the repository no-Docker rule
applies: Docker-dependent local gates are skipped and this single candidate is
pushed directly to `yang-arm` for official Cortex-A53 evaluation.

AI assistance covered the cross-function object-closure proof, SSA recurrence
matcher, fresh-index proof, invariant design, implementation, and documentation.
Technical basis: sparse duplicate aggregation, scalar replacement of a closed
linked representation, SSA recurrence recognition, effect/escape analysis, and
ordinary ARMv8-A integer/cache behavior.  No external implementation was copied
and no path, benchmark/source fingerprint, user-defined symbol or block name,
fixed judge input, runtime probe, or undefined behavior is used as a trigger.
