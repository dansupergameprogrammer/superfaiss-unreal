# Changelog

All notable changes to the SuperFAISSUnreal plugin. Versions follow the plugin's own
`.uplugin` `VersionName`, tracking (but not identical to) the vendored core's version —
see `Source/ThirdParty/SuperFAISS/VENDORED_VERSION.txt` for the exact core commit each
release vendors.

The format follows [Keep a Changelog](https://keepachangelog.com).

## [3.3.1] — 2026-07-22

Correctness release. The Bank Inspector now delivers the archive-inspection workflow the
3.2/3.3 UI described but did not fully reach — no new search or analysis primitive; every
change closes a gap between a claim the plugin made and what the reachable UI actually did.
The vendored core is unchanged from 3.3.0 (same `VENDORED_VERSION.txt`).

### Added
- **Visible "Open Archive…" actions on both the primary and second-bank slots.** A
  serialized scratch archive can be opened, replaced, and closed from the widget. The
  vendored core's `PeekScratchArchive` runs first, so the archive's geometry (rows, dims,
  metric, quantization, and the exact `archiveBytes` a load consumes) and any
  trailing-data or validation failure are shown *before* the load commits; a failed open
  leaves the current source untouched.
- **Novelty and channel-scoped analysis now run over archive sources**, not asset banks
  only, with the same tombstone exclusion and named-channel slicing the asset path applies.
- **Archive metadata in the shared source header** — live/published rows, metric,
  quantization, dimensions, channels, and filename — read from the inspection source
  rather than only a selected asset.
- **A CI capability-to-test matrix**, so a claim in the README, changelog, or a tooltip
  that no reachable UI delivers now fails the build rather than a human read.

### Changed
- **The correspondence classification threshold now ships a calibrated default** (`0.375`,
  previously a `0.0f` placeholder) with a written fixture-population basis. (Non-finite
  configuration already resolved to defined behavior; unchanged.)

### Fixed
- **Archive row identity now uses the original published source index in the Structure
  view's tree and PCA-scatter tooltip** — previously it fell back to the sample position
  when no asset was selected, so a pruned archive's rows now stay addressable by the
  identity the archive published. (The Correspondence view's match list already reported
  source indices directly from the core.)
- **Plugin claims reconciled with the reachable UI**, backed by a new CI
  capability-to-test matrix that asserts each advertised inspection capability has a
  reachable path and a test.

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
