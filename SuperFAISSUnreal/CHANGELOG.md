# Changelog

All notable changes to the SuperFAISSUnreal plugin. Versions follow the plugin's own
`.uplugin` `VersionName`, tracking (but not identical to) the vendored core's version —
see `Source/ThirdParty/SuperFAISS/VENDORED_VERSION.txt` for the exact core commit each
release vendors.

The format follows [Keep a Changelog](https://keepachangelog.com).

## [3.3.0] — 2026-07-21

### Changed
- **Vendored core bumped to v3.3.0** (see `Source/ThirdParty/SuperFAISS/CHANGELOG.md` and
  `VENDORED_VERSION.txt` for the full delta) — adds `PeekScratchArchive`/
  `ScratchArchiveInfo` (header-peek geometry + trailer-byte-offset read for a serialized
  scratch archive, not yet consumed by this plugin); fixes `ScratchBank::Load`'s retention
  validation, a segmented-scan/whole-row-scan kernel-selection disagreement at several
  `paddedDims`, an `int64` arena-size overflow in `ScratchBank::Create`/`Grow` when
  geometry ceilings were not enforced first, and a Cosine channel bank with zero rows
  being unrepresentable; and lets `MeanNNCrossDeviceChannel`/`MaxNNCrossDeviceChannel`
  return `OutOfMemory`.
- **`ComputePrincipalComponents`'s `scratch` parameter is now `double*`** (was `float*`)
  in the vendored core. This plugin's one call site
  (`SSuperFAISSBankInspector::ComputeProjection`, the PCA scatter-plot projection behind
  the Bank Inspector's Structure/Novelty views) updated its scratch buffer from
  `TArray<float>` to `TArray<double>` to match; no other behavior change.

### Fixed
- **Channel-table `Offset + Length` arithmetic widened to `int64` before every bounds
  check**, in `USuperFAISSVectorBank::InitFromSource`,
  `USuperFAISSVectorBank::RebuildChannelTable`, `USuperFAISSScratchBank::InitWithChannels`,
  and `USuperFAISSScratchBank::Relabel` — all four `BlueprintCallable` entry points. The
  caller-supplied `int32` sum could overflow before comparison, defeating the channel's own
  bounds and ascending-order checks. A malformed table was never actually accepted or
  stored — the vendored core's `ValidateBank` already rejects it downstream with
  `BadFormat` — but the plugin's own channel-rules guard was inert for the overflowing
  input, so the caller received a generic downstream failure instead of the specific
  diagnosis, and the signed overflow was undefined behavior regardless of reachability.
  Matches the core library's own fix for the same defect class in `ScratchBank::Create`
  and `Grow`.

## [3.2.1] — 2026-07-21

### Fixed
- Vendored core advanced to 3.2.1, which repairs the core test suite on Linux and
  macOS arm64 (an unguarded reference to x86-only kernel mirrors, and a stack buffer
  overrun in an allocation cell). The core library itself is byte-identical to 3.2.0,
  so plugin behaviour is unchanged; this release exists so the vendored tree and the
  version it reports are the repaired ones.

## [3.2.0] — 2026-07-20

### Added
- **Bank Inspector — structure, novelty, and correspondence.** A new editor tab (**Tools
  > SuperFAISS Bank Inspector**) built on the vendored core's `graph.h`/`novelty.h`/
  `matching.h` primitives, answering three questions a raw query can't:
  - **View A — Structure.** Clusters the current sample by mutual k-nearest-neighbor
    agreement plus exact-duplicate grouping, then connected components. One result row
    per cluster (plus an Outliers row below the configured minimum size), expandable to
    each member's bank id. Selecting a cluster highlights every one of its points in the
    PCA scatter; selecting a single row highlights the row itself, its k-nearest
    neighbors, and the rest of its cluster as three distinct signals.
  - **View B — Novelty.** A probe box (row id or `#index`) answers duplicate / familiar
    / novel against the bank's own content — an exact-distance identity check first (a
    true 0.0 metric distance on a real duplicate under Cosine int8 or a byte-identical
    float32 row; a disclosed double-precision epsilon on L2's expanded form), falling
    through to a statistical rank against a calibrated k-th-neighbor baseline only when
    the identity check does not already resolve it. An evidence list shows the probe's
    actual nearest neighbors with scores and margins.
  - **View C — Correspondence.** Given a second bank (asset picker, or an opened scratch
    archive), reports each sampled row of the primary bank's matched partner in the
    second bank, with a CSLS margin and a matched/ambiguous state. Disclosed as the
    HEAVY pass in the set — cost scales with both banks' sizes — before it runs.
  - **Open scratch archive…**, on both the primary and second-bank slots, loads a saved
    `USuperFAISSScratchBank` archive file directly as a transient inspection source, with
    no need to bake it to an asset first. Tombstoned rows are honored throughout — never
    sampled, clustered, or reported as a match.
  - **Analysis parameters** (sample cap, structure's k and minimum cluster size,
    novelty's k and lambda, correspondence's match-k and CSLS threshold) live in one
    per-user, per-project editor settings object and persist across sessions.
- **Insights instrumentation bar.** Every analysis pass, and the runtime query path
  underneath it, is wrapped in a dedicated, zero-cost-when-off `SuperFAISS` trace channel
  for Unreal Insights, plus a `STATGROUP_SuperFAISS` stat group (bytes streamed,
  effective bandwidth, chunk count, batch size, per-query time, queries in flight, and
  the zero-steady-state-allocation counters). The determinism suite runs trace-OFF and
  trace-ON and asserts one identical result, so a profiling session cannot itself change
  the answer.

### Changed
- **Vendored core bumped to v3.2.0** (see `Source/ThirdParty/SuperFAISS/CHANGELOG.md` and
  `VENDORED_VERSION.txt` for the full delta) — the `graph.h`/`novelty.h`/`matching.h`/
  `inspector_common.h` headers the Bank Inspector is built on, plus the release-identity
  correction to `version.h` and the documentation of `NoveltyProbeDistance`'s off-grid
  int8 channel rejection.

### Fixed
- **Novelty panel's `duplicate` verdict no longer renders a meaningless CDF readout.**
  A `duplicate` verdict is decided entirely by the exact-identity check (limb 1); the
  statistical-rank baseline (limb 2) is never consulted for it and its fields stay at
  their zeroed defaults. The rendered text previously printed them anyway
  (`duplicate — novelty 0.0000 vs 0 of 0 sampled rows`); it now renders the plain
  verdict word for this case.

## [3.1.2] — 2026-07-18

### Fixed
- A release-hardening point, prompted by an external review. Fixes the Win64 **Shipping**
  game build (a missing `MassEntityQuery.h` include in the demo module's swarm processor —
  the Editor PCH masked the IWYU gap); scrubs stale public contracts that survived 3.1 (the
  scratch channel table is mutable via `Relabel`, and scratch channel queries resolve —
  headers, `API.md` and `INTEGRATION.md` now say so); documents `Relabel` in the core
  API/integration docs; corrects the core version header, which still reported 3.0.1; and
  narrows the plugin README's portability claim to the surfaces actually verified.

## [3.1] — 2026-07-18

### Added
- **A runtime-mutable channel vocabulary.** `Relabel` re-partitions the channel table on a
  live scratch bank — add or remove channels, change their count *and* boundaries, or
  promote a single-space bank to channels and demote it back — without a rebuild. The rows
  are unchanged; only the partition moves. It is exclusive like `Grow`/`Load` (it drains
  in-flight queries) and reject-over-degrade (a malformed table leaves the bank exactly as
  it was), and a relabeled bank scores identically to a fresh bank created under the new
  table over the same rows.
- The Blueprint channel-scratch surface completes alongside it — named-channel scratch
  queries, per-channel scratch recall, and the channel vocabulary surviving a save/load
  round trip — as do read-only MCP closures of the analytics reductions (spread and
  mean/max nearest-neighbour) over a live snapshot.

### Fixed
- A segmented-kernel AVX2 defect: a length-4 channel could score a spurious `0` on the
  AVX2 float path.

## [3.0.1] — 2026-07-13

### Fixed
- The `SUPERFAISS_VERSION_*` macros reported the wrong version; they now report 3.0.1.
- A zero-energy Cosine channel edge: a channel carrying no energy on a valid
  (whole-row-normalized) row now floors to a defined `0` in the NN-divergence reductions
  rather than being rejected, and the per-channel recall audit excludes such a sampled row
  instead of aborting.

## [3.0] — 2026-07-13

### Added
- **Channel-capable scratch banks and channel-scoped analytics.** Channels — the named
  sub-space partition baked banks have carried since 2.0 — extend to the mutable half.
  `InitWithChannels` fixes a channel table on a scratch bank at construction; a
  named-channel scratch query ranks a snapshot by a weighted combination of channels and
  agrees **bit-for-bit** with its baked twin; channel-aware `Freeze` graduates the bank to a
  schema-2 channel bank; and the per-channel recall audit reports recall@k per channel.
- Every 2.5 analytics operator gains a channel-scoped form, on Blueprint and read-only MCP.
  The cost is append-time only (the Cosine per-channel sub-norm), never on the query path.

## [2.5] — 2026-07-12

### Added
- **Bank analytics** — cross-device-deterministic reductions over int8 banks: a set-to-set
  centroid distance, directed nearest-neighbour set divergence (mean, and the order-free max
  that is the Hausdorff component), within-bank dispersion, and the shared query-vs-query
  pair score they rest on. All are bit-identical across machines by the same
  integer-accumulation proof as the query path, on the Blueprint subsystem and as read-only
  MCP tools.
- An offline per-device probe-direction projection report.
- The editor **Bank Inspector** (Tools > SuperFAISS Bank Inspector) shows a live PCA
  projection beside its ranked-query view.

### Changed
- The result-direction convention (a `Dot` bank returns a similarity, not a distance) and the
  cosine limb's determinism condition are now stated in the docs.

## [2.4] — 2026-07-11

### Added
- **Integer-domain pooling.** `MakeCentroidQueryCrossDevice` pools int8 rows into a
  *quantized* cross-device query — order-free integer accumulation, no float mean — so pooled
  queries honestly participate in cross-device-exact results. `QueryPooledCrossDevice`
  executes exactly those bytes, and the editor bakes the same operator's product into
  cross-device-tier prototype assets, with a baked anchor byte-equal to a runtime pool over
  identical rows, behind a required asset version bump.
- Pooled recall is measured beside the operator and stated with its conditions.

## [2.3] — 2026-07-11

### Added
- **Scratch-bank recall audit.** An opt-in float-retention posture on scratch banks — never
  the default, and the memory cost is stated plainly — plus `MeasureRecall`, reporting the
  bank's own cross-device recall with a seed, a generation stamp and a stale mark. Any later
  append, remove or load marks the report stale, never silently current.
- `DescribeScratchBank` (MCP) reports it read-only.

## [2.2] — 2026-07-04

### Added
- **Cross-device bit-exactness.** An opt-in query mode for int8 banks returning bit-identical
  scores and hit order on any machine at any SIMD width — the contract lockstep/rollback
  multiplayer and networked motion matching require. The claim runs as a CI test against
  committed fixtures, it measures faster than the default int8 scan, and the importer reports
  the mode's recall beside standard recall on every bank asset.

## [2.1] — 2026-07-04

### Added
- **Per-row score bias, in-scan and exact.** Sparse (index, bias) pairs for motion matching's
  continuing-pose reward (effectively free) and a dense per-row view for memory salience
  (+3.5% f32 / +1.9% int8, measured). Finite-only; exclusion beats bias; rewards are negative
  on L2.

## [2.0] — 2026-07-04

### Added
- **Named channels.** Rank by a weighted combination of vector sub-spaces, with exact
  per-channel decomposition of every hit and true per-channel cosines. Channels are a
  semantics feature, not a speed feature: a channel query costs approximately one full scan.
- **Scratch banks.** Mutable runtime banks — append, remove, query, freeze, save — for NPC
  memory and session-accumulated embeddings.

---

Releases `v1.0`–`v1.2` and the `v2.2.1`–`v2.2.3` patches are tagged in this repository but
predate this changelog and were never described on a public surface; their content is in the
git history for those tags.
