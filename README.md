# SuperFAISS For Unreal Engine

Fast, deterministic, allocation-free k-nearest-neighbor search for Unreal Engine
5.8+ — over banks you import as assets or grow at play time. One
`USuperFAISSVectorBank`/`USuperFAISSScratchBank` pair answers many questions:
whole-vector similarity or any weighted mix of named channels, decomposition bars
for *why* each hit ranked, gameplay state folded into the ranking in-scan,
prototype categories pooled from selected rows, with the quantization cost measured and stored on every
bank asset. Exact, bit-deterministic per device, and (opt-in) bit-identical across
machines. This is the reference engine integration of the MIT-licensed
[SuperFAISS](https://github.com/dansupergameprogrammer/superfaiss) core
library (vendored — no external dependency).

**Current release: [v3.3.0](https://github.com/dansupergameprogrammer/superfaiss-unreal/releases/tag/v3.3.0)**, bundling core [v3.3.0](https://github.com/dansupergameprogrammer/superfaiss/releases/tag/v3.3.0) — see [CHANGELOG.md](SuperFAISSUnreal/CHANGELOG.md), and [VENDORED_VERSION.txt](SuperFAISSUnreal/Source/ThirdParty/SuperFAISS/VENDORED_VERSION.txt) for the exact core commit. Version markers in this file record when a capability landed, not the current version.

SuperFAISS is an **independent implementation** — not a fork of, derived from, or
affiliated with Meta's FAISS; the name is nominative homage.

Embeddings turn meaning into geometry: words, NPC memories, items, sounds — anything an
encoder model can vectorize — become points where *similar means near*. This plugin
answers "what's most similar to this?" exactly, in microseconds, on any thread.

A single query against the shipped 40,000-word demo bank returns in **0.13 ms**, exact and
bit-deterministic, with zero steady-state allocation — see [Measured](#measured-desktop-editor-shipped-demo-bank-40000-words-x-100-dims-int8-4-mb)
for the full numbers and how to reproduce them.

## Try it

1. Clone. Open `ExampleProject/ExampleProject.uproject` (UE 5.8+; the project finds
   the plugin in the repo root via `AdditionalPluginDirectories`). Build when prompted.
2. The editor opens on the demo map. Play in editor, type a word at it.

Or headless, from the repo root:

```
UnrealEditor-Cmd ExampleProject/ExampleProject.uproject -ExecCmds="Automation RunTests SuperFAISS; Quit" -unattended -nullrhi
```

104 automation tests (108 with the MCP toolset plugin enabled): kernel correctness,
SIMD/scalar mirror equality, determinism, tie-break stability, concurrency, asset
round-trips, import rejection, quantizer recall, performance guards, query composition
(centroid, direction, intersection, margins), named-channel queries and decomposition,
per-row bias, scratch banks, **channel-capable scratch banks (v3.0) — a named-channel
scratch query proven bit-equal to its baked twin, channel-aware freeze to a schema-2
bank, per-channel recall, the composition set (bias / cross-device / tombstone), and
the rejection catalog; channel-scoped analytics; the channel-scratch linter**,
cross-device exactness (v2.2 — including a golden-hash battery over committed fixtures
that must match the core CI's pin), bank analytics (set-to-set distance,
drift/divergence/spread, projection — v2.5), bank lint analyses, prototype authoring, a
golden semantic query on the demo bank, and the Mass swarm's stability.

## What it does

One bank asset, one subsystem, and a set of questions that grows over time. Version
markers record when a capability landed, not the current version; the per-release history
is in [CHANGELOG.md](SuperFAISSUnreal/CHANGELOG.md).

- **Exact top-k over baked bank assets (v1.0).** Import embeddings as a
  `USuperFAISSVectorBank`, query from Blueprint or C++ on any thread. Exact search, not
  approximate — no index to tune, no recall cliff. The importer measures and stores the
  quantization cost on the asset itself, so the recall number travels with the bank.
- **Named channels (v2.0).** Rank by a weighted combination of vector sub-spaces —
  "similar in *identity*, ignoring *appearance*" — with exact per-channel decomposition of
  every hit and true per-channel cosines. A semantics feature, not a speed feature: a
  channel query costs approximately one full scan, and the docs say so plainly.
- **Scratch banks (v2.0).** Mutable runtime banks — append, remove, query, freeze, save —
  for NPC memory and session-accumulated embeddings. They grow during play and survive a
  save game.
- **Per-row score bias, in-scan and exact (v2.1).** Sparse (index, bias) pairs for motion
  matching's continuing-pose reward, effectively free; a dense per-row view for memory
  salience (+3.5% f32 / +1.9% int8, measured). Finite-only; exclusion beats bias.
- **Cross-device bit-exactness (v2.2).** An opt-in int8 query mode returning bit-identical
  scores and hit order on any machine at any SIMD width — the contract lockstep/rollback
  multiplayer and networked motion matching require. It runs as a CI test against committed
  fixtures, and measures *faster* than the default int8 scan.
- **Recall you can audit (v2.3).** An opt-in float-retention posture on scratch banks —
  never the default, and the memory cost is stated plainly — plus `MeasureRecall`, which
  reports the bank's own cross-device recall with a seed, a generation stamp and a stale
  mark. Any later append, remove or load marks the report stale, never silently current.
- **Integer-domain pooling (v2.4).** `MakeCentroidQueryCrossDevice` pools int8 rows into a
  *quantized* cross-device query — order-free integer accumulation, no float mean — so
  pooled queries honestly participate in cross-device-exact results, and a baked prototype
  anchor byte-equals a runtime pool over identical rows.
- **Bank analytics (v2.5).** Cross-device-deterministic reductions over int8 banks:
  set-to-set centroid distance, directed nearest-neighbour divergence (mean, and the
  order-free max that is the Hausdorff component), within-bank dispersion. Drift over
  checkpoints is one operator between two checkpoints' row sets. Blueprint and read-only
  MCP.
- **Channels on the mutable half (v3.0), and a runtime-mutable vocabulary (v3.1).**
  `InitWithChannels` fixes a channel table on a scratch bank; a named-channel scratch query
  agrees **bit-for-bit** with its baked twin. `Relabel` then re-partitions that table on a
  live bank — add, remove, or move channel boundaries, promote a single-space bank to
  channels and demote it back — without a rebuild. Exclusive like `Grow`/`Load`, and
  reject-over-degrade: a malformed table leaves the bank exactly as it was.
- **The Bank Inspector (v3.2).** Reading a bank's structure, not only querying it: a mutual
  k-NN neighbour graph with connected components, a two-limb novelty verdict
  (`duplicate` / `familiar` / `novel`) combining exact metric-distance identity with a rank
  against a calibrated baseline, sampled mutual correspondence between two banks with CSLS
  margins, and a PCA scatter — over a bank asset or a serialized scratch archive. Every pass
  is sampled and discloses its coverage. These passes are **per-device** deterministic; the
  cross-device claim covers the query path, and the panel says so.
- **Enforced allocation guarantees (v3.2).** Every public entry point carries an allocation
  cell under a raw-allocation tracking seam, and a registry check fails the build when a new
  entry point arrives without one. The guarantee is enforced rather than stated.
- **Archive peek and bounded geometry (v3.3).** The vendored core gains
  `PeekScratchArchive` — read a serialized archive's geometry, and the exact byte length a
  load consumes, without allocating or reading the payload, so a host that appends its own
  trailer can validate it before committing the load. Scratch `Create`/`Grow` bound their
  geometry before computing an arena size, and a loaded bank's retention region is validated
  against the rows it claims to describe.

![The shipped demo: one query against two GloVe banks, sub-millisecond async results](docs/demo.png)

The editor **Bank Inspector** (Tools > SuperFAISS Bank Inspector) reads a bank's
structure rather than only querying it: mutual k-NN clustering, a novelty verdict
for a probe row against a calibrated baseline, mutual correspondence between two
banks with CSLS margins, and a PCA projection — over a bank asset or a serialized
scratch archive. Every pass is sampled and says so, and the per-device determinism
scope is stated in the panel itself.

![The Bank Inspector over a 10,418-row pose bank: ranked query results with score and margin, a novelty verdict reading "familiar" against 2,048 sampled rows, the cluster list from a mutual k-NN structure pass, correspondence against a second bank, and the PCA scatter with an outlier selected](docs/inspector.png)

## Use it in your project

Copy `SuperFAISSUnreal/` into `<YourProject>/Plugins/` and enable it. The plugin is
self-contained (the core library is vendored inside), has no platform allowlist, and
depends only on stable engine modules (the demo module adds Slate/InputCore and
Mass — UE 5.8 engine modules, no plugin references — and is strippable in three
steps) — see the [plugin README](SuperFAISSUnreal/README.md) for
the API quick start, bank authoring, guarantees, and demo-strip steps.

**Designed for portability, verified on one target.** No platform code, no platform
allowlist — SIMD is selected automatically. What is *verified* today: the plugin on
UE 5.8 Win64 (Editor + automation suite + Shipping game build); the vendored core's own
CI additionally covers Windows/Linux (AVX2, TSan) and Apple Silicon NEON. Other UE
targets should work by construction but have not been qualified.

## Engine versions

Developed and tested on UE 5.8. The code surface is deliberately conservative —
engine subsystem, `ParallelFor`/task graph, standard UObject serialization, Slate —
so older engines are expected to be a recompile, with two version-sensitive points:

- The Build.cs files set `FPSemantics = FPSemanticsMode.Precise`, which the
  determinism guarantee requires (source pragmas do not stop clang FP contraction).
  Engines predating that UBT property need an equivalent per-module precise-FP flag.
- The shipped `Content/Demo` assets are saved by UE 5.8 and will not load in older
  engines (UE packages are not backward-compatible). Regenerate them locally:
  bake a bank from any sidecar-format source with `-run=SuperFAISSUnrealBake`
  (the shipped bank uses [GloVe](https://nlp.stanford.edu/projects/glove/) 6B 100d),
  then rebuild the demo map with `-run=SuperFAISSUnrealDemoMap`.
- The example project's `BuildSettingsVersion`/`EngineAssociation` may need a
  per-version bump.

## Repo layout

| Path | What |
|---|---|
| `SuperFAISSUnreal/` | The plugin. Copy this folder into your project's `Plugins/`. |
| `SuperFAISSUnrealMCP/` | Optional MCP toolset: enumerate, describe, query, import, validate, and lint tools over your banks. Requires Experimental engine plugins most distributions don't carry — see its README. Disabled by default; everything else works without it. |
| `ExampleProject/` | Minimal host project; opens straight into the demo map. |

## Documentation

- [plugin README](SuperFAISSUnreal/README.md) — API quick start, bank authoring,
  guarantees, and the demo-strip steps
- [CHANGELOG.md](SuperFAISSUnreal/CHANGELOG.md) — the per-release history
- [VENDORED_VERSION.txt](SuperFAISSUnreal/Source/ThirdParty/SuperFAISS/VENDORED_VERSION.txt)
  — the exact core commit this release vendors, and what it carries
- Core library docs, vendored alongside the source:
  [API](SuperFAISSUnreal/Source/ThirdParty/SuperFAISS/docs/API.md) ·
  [DETERMINISM](SuperFAISSUnreal/Source/ThirdParty/SuperFAISS/docs/DETERMINISM.md) ·
  [FORMAT](SuperFAISSUnreal/Source/ThirdParty/SuperFAISS/docs/FORMAT.md) ·
  [INTEGRATION](SuperFAISSUnreal/Source/ThirdParty/SuperFAISS/docs/INTEGRATION.md)

## Measured (desktop editor, shipped demo bank: 40,000 words x 100 dims, int8, ~4 MB)

| Path | Per query | Notes |
|---|---|---|
| Single query | **0.13 ms** | auto-parallelized across chunks |
| Batched | **0.06 ms per query** | one pass over the bank |
| Core serial scan, one core | ~0.65 ms | the same bank through the core's own bench |

Exact search, bit-deterministic, zero steady-state allocation.

## The original pitch

v1.0's opening pitch, word for word:

> Fast, deterministic, allocation-free k-nearest-neighbor search over baked embedding
> banks, for Unreal Engine 5.8+. This is the reference engine integration of the
> MIT-licensed [SuperFAISS](https://github.com/dansupergameprogrammer/superfaiss) core
> library (vendored — no external dependency).

Every word still holds — the plugin just answers more questions now. The banks no
longer have to be baked (scratch banks grow during play and survive save games),
and one bank now serves channel-weighted queries, decomposition, in-scan bias,
prototype categories, and honest per-bank recall numbers alongside the plain top-k
it started with.

## License

- Plugin and example project: MIT (see `LICENSE`).
- SuperFAISS core: MIT (`SuperFAISSUnreal/Source/ThirdParty/SuperFAISS/LICENSE`).
- Demo bank vectors: GloVe (Pennington, Socher, Manning; Stanford NLP), Public Domain
  Dedication and License v1.0.
