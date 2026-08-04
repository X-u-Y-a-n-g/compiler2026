# Baseline record

## Identity

- Name/version: accepted AArch64 delivery baseline
- Source repository or artifact: local compiler repository and compiler-impl2026-dev
- Code commit: 063502f (optimization content identical to accepted 233b204)
- Dataset/workload version: 2026 ARM performance suite, 60 cases
- Evaluator/metric implementation: repository ARM runner using /workspace/build/compiler
- Created or last run at: 2026-07-30

## Strategy and status

- Strategy: existing-local
- Status: attested
- Intended use: idea screening and competition promotion comparator
- Confidence required for intended use: full local gates before push; official Cortex-A53 result for keep/revert

## Execution evidence

- Exact command: repository ARM 60-case -O1 validation in compiler-impl2026-dev (fresh candidate gate pending)
- Environment/dependencies: compiler-impl2026-dev; /workspace/build/compiler; aarch64-linux-gnu-gcc; qemu-aarch64
- Hardware: local QEMU diagnostics; official Cortex-A53 owned submission
- Seed/fold/workload: deterministic 60 performance cases
- Runtime and compute cost: staged; full gate required before candidate publication
- Logs: prior task records and pushed git history
- Checkpoints/predictions/results: previous scalar-residency candidate 34.48 s versus parent 34.35 s official aggregate and was reverted
- Artifact hashes: git commit identity above
- Reported metric: accepted baseline remains parent behavior

## Verification performed

- [ ] Official evaluator understood and smoke-tested
- [ ] Artifact loads or inference executes
- [ ] Saved predictions/results recompute to the reported metric
- [ ] Tiny or representative workload agrees
- [ ] Environment and dependency assumptions inspected
- [ ] Full clean reproduction agrees within stated tolerance

## Deferral and risk

- Why full reproduction is deferred, if applicable:
- Estimated full time/compute:
- Substitute checks:
- Residual risk:
- Trigger that requires full reproduction:
- Expiry/staleness condition:

## Attestation

- Evidence came from an actual run: yes
- Recorded by/date: Codex with repository owner evidence / 2026-07-30
- Independent reproduction: no
