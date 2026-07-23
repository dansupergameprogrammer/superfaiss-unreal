# SuperFAISSUnreal

Fast, deterministic, allocation-free k-nearest-neighbor search for Unreal Engine
5.8+ — over banks you import as assets or grow at play time. One
`USuperFAISSVectorBank`/`USuperFAISSScratchBank` pair answers many questions:
whole-vector similarity or any weighted mix of named channels, decomposition bars
for *why* each hit ranked, gameplay state folded into the ranking in-scan,
prototype categories pooled from selected rows, mutable banks that grow during play
and survive save games — with the quantization cost measured and stored on every
bank asset. Exact, bit-deterministic per device, and (opt-in) bit-identical across
machines. Built on the MIT-licensed
[SuperFAISS](https://github.com/dansupergameprogrammer/superfaiss) core library
(vendored under `Source/ThirdParty/SuperFAISS`).

SuperFAISS is an **independent implementation** — not a fork of, derived from, or
affiliated with Meta's FAISS; the name is nominative homage.

Embeddings turn meaning into geometry: words, NPC memories, items, sounds — anything an
encoder model can vectorize — become points where *similar means near*. This plugin
answers "what's most similar to this?" exactly, in microseconds, on any thread.

Measured on the shipped demo bank (40,000 words × 100 dims, int8, ~4 MB), desktop editor:
single query **0.13 ms** (auto-parallelized across chunks — the core's serial one-core
scan of the same bank is ~0.65 ms), batched **0.06 ms per query** — exact search,
bit-deterministic, zero steady-state allocation.

## Quick start

1. Enable the plugin. Banks are `USuperFAISSVectorBank` assets; the shipped example is
   `/SuperFAISSUnreal/Demo/DemoBank` (GloVe word vectors, public domain).
2. Query from anywhere:

```cpp
USuperFAISSSubsystem* Sim = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();

// Blocking (any thread; ~microseconds on game-scale banks).
TArray<FSuperFAISSHit> Hits;
FSuperFAISSQueryArgs Args;
Args.K = 10;
Sim->QuerySync(Bank, QueryVector, Args, Hits);   // Hits: Index, Id (FName), Score

// Non-blocking: task graph, bank pinned against GC, delegate on the game thread.
FSuperFAISSNativeResultDelegate Done;
Done.BindLambda([](const TArray<FSuperFAISSHit>& Hits, bool bOk) { /* ... */ });
FSuperFAISSTicket Ticket = Sim->QueryAsync(Bank, QueryVector, Args, MoveTemp(Done));
// Ticket.Cancel() is best-effort.

// Many queries, one bank pass (crowds, Mass processors):
Sim->QueryBatch(Bank, ConcatenatedQueries, QueryCount, Args, Hits, Counts);
```

Blueprint: `Query Similar (Sync)` / `Query Similar (Async)` on the subsystem.

## Composing queries

Every hit carries `Margin` — the score gap to the next-ranked hit ("won by how
much"); the top hit's margin is the runner-up gap. Beyond a raw vector, queries can
be built and combined:

```cpp
// "The category's center": mean of selected rows (int8 dequantized; Cosine
// renormalized; a cancelling mean is rejected, not renormalized into noise).
Sim->MakeCentroidQuery(Bank, {RowA, RowB, RowC}, Centroid);

// "Most X-like relative to Y": unit direction, ranked as a dot query.
Sim->MakeDirectionQuery(VectorX, VectorY, Direction);
Args.bScoreAsDot = true;                    // required on L2 banks; no-op otherwise
Sim->QuerySync(Bank, Direction, Args, Hits);

// "Similar to ALL of these": exact intersection in one bank pass. Each row is
// ranked by its WORST score against the member queries, so every returned row
// clears the fused score against every query. Subtraction is the exclusion bitset.
Sim->QueryIntersect(Bank, ConcatenatedQueries, QueryCount, Args, Hits);
```

Blueprint: `Make Centroid Query`, `Make Direction Query`, `Query Similar (Intersect)`.

## Named channels and decomposition (v2.0)

Banks whose vectors carry heterogeneous sub-spaces — "dims 0–63 are identity,
64–99 are appearance" — can declare **named channels** in the sidecar
(`schemaVersion: 2`). Queries then rank by a weighted combination of channels,
and every hit can be decomposed into exact per-channel contributions:

```cpp
FSuperFAISSQueryArgs Args;
Args.K = 10;
Args.Channels = {{TEXT("identity"), 1.0f}, {TEXT("appearance"), 0.2f}};
Sim->QuerySync(Bank, QueryVector, Args, Hits);

// "Why did this match?" — contributions sum EXACTLY to the hit's score
// (they are the scan's own accumulators, not a re-computation).
TArray<float> Contributions;
float Total;
Sim->DecomposeHit(Bank, QueryVector, Args.Channels, Hits[0].Index, Contributions, Total);
```

On Cosine channel banks each channel's score is a **true per-channel cosine**
(per-row inverse sub-norms are baked from the quantized payload at import, and
the query's channel sub-vectors renormalize at query build). Weight 0 mutes a
channel; raw element ranges (`Args.Segments`) are the C++ escape hatch.

**Honest pricing:** segments are a *semantics* feature. A channel query costs
approximately one full scan — for dot-family scoring the weights fold into the
query exactly, so it runs the plain V1 kernels at V1 speed; masking a range
does not make the scan faster on this row-interleaved layout (measured, not
assumed). Per-channel recall@10 is measured at import per channel and stored
on the asset; the `-Lint` pass reports channels too weak to trust
(sub-norm energy floor), channel-scoped near-duplicates, and degenerate
channels.

Blueprint: `Query Similar (Channels)`, `Decompose Hit`. The Bank Inspector
gets per-channel weight sliders, decomposition bars on every hit, and
channel-scoped PCA projection.

## Scratch banks (v2.0)

`USuperFAISSScratchBank` is a mutable, fixed-capacity bank for vectors that
accumulate at runtime — NPC memories, session embeddings. It is runtime state,
not an asset:

```cpp
USuperFAISSScratchBank* Memory = NewObject<USuperFAISSScratchBank>(Owner);
Memory->Init(/*Capacity*/ 512, Dims, ESuperFAISSBankMetric::Cosine,
	ESuperFAISSBankQuantization::Float32);
int32 Index;
Memory->Append(Vector, Index);      // validates like the importer; index is stable
Memory->Remove(Index);              // tombstone; removal is exclusion on later queries
Sim->QueryScratch(Memory, QueryVector, Args, Hits);   // any thread, lock-free

TArray<uint8> SaveBlob;
Memory->SaveToBytes(SaveBlob);      // full state; LoadFromBytes rejects bad blobs
TArray<int32> IndexMap;
USuperFAISSVectorBank* Frozen = Memory->Freeze(IndexMap);  // graduate to content
```

One logical writer at a time; queries are lock-free readers on any thread and
pin the bank for their flight. `Grow` preserves indices (stored indices are
the contract); `Freeze` compacts and renumbers, returning the old-to-new map
so stored handles survive. A frozen bank scores **bit-identically** to the
scratch bank it came from. Determinism extends as determinism-given-history:
same append/remove sequence + same query = bit-identical results per device.

Blueprint: the full surface is BlueprintCallable, plus `Query Similar (Scratch)`.

## Channel-scoped scratch banks and mutable vocabulary (v3.0 / v3.1)

A scratch bank can carry the same **named channels** a baked bank does. `InitWithChannels`
declares the channel table (names + element ranges); `QueryScratch` then resolves named
channels against the bank's own vocabulary — on Cosine banks each channel's score is a
true per-channel cosine, identical to the equivalent raw-range query. `MeasureRecallPerChannel`
reports recall per channel, and the analytics operators (`Bank Spread`, `Mean`/`Max Nearest
Neighbor`, `Set-To-Set Distance`) all have scratch closures that run over a live snapshot.

```cpp
Memory->InitWithChannels(512, Dims, ESuperFAISSBankMetric::Cosine, Quant,
    /*Names*/ {"identity", "appearance"}, /*Offsets*/ {0, 64}, /*Lengths*/ {64, 64});
Sim->QuerySimilarChannelsScratch(Memory, Query, {{"identity", 1.0f}}, K, Hits);
```

The channel vocabulary is **mutable at runtime via `Relabel`** — re-partition the channels of
a live bank (add or remove channels, change their count *and* boundaries, or promote a
single-space bank to channels and demote it back) without a rebuild. The rows are unchanged;
only the partition moves. `Relabel` is exclusive like `Grow`/`Load` (it drains in-flight
queries), and reject-over-degrade: a malformed table leaves the bank exactly as it was. A
relabeled bank scores identically to a fresh bank created under the new table over the same
rows. Read-only MCP tooling is unaffected — the vocabulary is mutated by game code, never
over MCP.

Blueprint: `Query Similar (Scratch, Channels)`, `Relabel`, and the scratch analytics entry
points are all BlueprintCallable.

## Per-row bias (v2.1)

An optional per-row score bias applied **in-scan**, so the composed ranking is
exact — built for shapes like motion matching's continuing-pose reward:

```cpp
FSuperFAISSQueryArgs Args;
Args.K = 10;
Args.BiasPairs = {{ContinuingPoseRow, +0.15f}};   // one biased row per query: free
// or dense, one float per bank row (memory salience — strength x recency):
// Args.RowBias = SalienceArray;                  // exactly Count floats, else rejected
Sim->QuerySync(Bank, QueryVector, Args, Hits);

// DecomposeHit reports bias as a visible separate term:
Sim->DecomposeHit(Bank, QueryVector, Channels, Hits[0].Index, Contributions, Total,
	/*RowBias*/ 0.15f);   // Contributions sum + RowBias == Total
```

Rules, stated plainly: values must be finite (`-inf` is not a mask — use
`ExcludeBits`; exclusion beats bias); rewards are positive on Dot/Cosine and
**negative on L2** (lower is better); a dense view must be exactly `Count` floats
(on scratch banks, the snapshot's count — stale saliences are rejected, never
silently misaligned); on `QueryIntersect` the bias applies once, to the fused
score. Cost: sparse is effectively free (+0.4% single, ~0% batch); dense measures
+3.5% f32 / +1.9% int8 single — and a per-query dense view in a batch streams
M x Count x 4 bias bytes beside the bank, which is why the sparse form exists.

## Cross-device exactness (v2.2)

An opt-in query mode promising **bit-identical scores and hit order across
different machines** — x86 and ARM, Windows/Linux/macOS, any SIMD width — the
property lockstep and rollback multiplayer, networked motion matching, and
server-side validation actually require. The contract is not aspirational
prose: it runs as a CI test, a pinned cross-device hash asserted on every push.

```cpp
FSuperFAISSQueryArgs Args;
Args.K = 10;
Args.bCrossDeviceExact = true;      // int8 banks only; composes with everything
Sim->QuerySync(Bank, QueryVector, Args, Hits);
// Blueprint: Query Similar (Cross-Device Exact)
```

How it holds: the query is quantized to int8 (round-half-even in integer math)
and scoring accumulates in integers — integer addition is associative, so
reduction width cannot matter; the per-row epilogue is one fixed-order
double-precision expression ending in an explicit subnormal floor (any final
score with magnitude below `FLT_MIN` is exactly `0.0f` on every machine — the
FTZ/DAZ hole closed by contract). The core's CI asserts a pinned hash over
committed fixture banks — including adversarial tiny-scale banks aimed at the
subnormal window — on Windows, Linux, and macOS-ARM across every kernel path
each runner can force, and the `SuperFAISS.B.CrossDeviceGoldenHash` automation
test asserts the UE-compiled core reproduces the same pin.

Stated plainly: f32 banks are refused in this mode (per-device only); query
quantization adds recall cost beyond row quantization — the importer measures
**cross-device recall@10 beside standard recall@10** and stores both on the
bank asset; cross-device scores are not equal to default-mode scores (different
math). Cost: the integer path measures FASTER than the default int8 scan
(−18.5% single / −14.6% batch at 100k×256, desktop AVX2). `DecomposeHit` takes
`bCrossDeviceExact` and matches the cross-device scan score bitwise. Full
contract: vendored `DETERMINISM.md` §2c.

## Scratch recall audit (v2.3)

`Init(..., bRetainFloats=true)` opts a scratch bank into a dev/audit posture that
retains the post-normalization float rows beside the quantized rows (an int8
256-dim row grows ~4.9x — stated, not assumed), and `MeasureRecall` then reports
the bank's own cross-device recall@10 against that retained reference, with the
sample count, seed, and a generation stamp; any later append, remove, or load
marks the report stale, never silently current. `FreezeWithRecall` re-measures
over the compacted rows at freeze time, and `DescribeScratchBank` (MCP) states
the flag and the report read-only.

## Integer-domain pooling (v2.4)

`MakeCentroidQueryCrossDevice` pools int8 bank rows into a **quantized**
cross-device query (order-free integer accumulation — no float mean), so pooled
queries can honestly participate in cross-device-exact results;
`QueryPooledCrossDevice` executes exactly those bytes, and all-equal salience
weights produce the bit-identical unweighted payload. Stated plainly: the
executor is a K-only surface — it takes the payload and a K; exclusion, bias,
and channels do not compose through this entry point (the core `QueryXd` path
supports exclusion and bias; the plugin surface exposes them when a consumer
needs them, not before). The payload validates on execution: a non-finite scale
or a self-dot that isn't the image's own is a rejection, never a silently wrong
ranking. The editor's `CreatePrototypeAssetCrossDevice` bakes the same
operator's product into a cross-device-tier prototype asset (required asset
version bump — an XD payload under the old version is refused), so a baked
anchor byte-equals a runtime pool over identical rows; float prototype assets
remain the presentation tier, unchanged. Pooled recall is measured beside the
operator at calibration (0.9930 / 0.9885 / 0.9895 recall@10, Dot / Cosine / L2 —
200 seeded 8-row pools over the core suite's 512×128 int8 fixtures, fixed
recorded seed); measure per bank before adopting, as with any quantized mode.

## The query side of the encoder seam

An *encoder* is anything that turns domain state — text, images, gameplay — into a
vector. This plugin ships no encoders, ever: the bake side of the seam is the sidecar
format (any pipeline that emits it is an encoder), and the query side is
`ISuperFAISSQueryProvider` — implement `GetQueryVector(Bank, OutQuery)` on any object
(C++ or Blueprint) and hand its output to the subsystem. The shipped reference
implementation, `USuperFAISSBankRowQueryProvider`, produces the query from an
existing bank row by Id or index — the "find things like this known thing" case.
Everything domain-specific lives on the implementing side of the interface.

## Making banks

Export embeddings from any pipeline as the two-file sidecar format —
`<name>.wvbank.json` (header) + `<name>.wvbank.bin` (raw float32 rows; a few lines to
emit from Python, see SuperFAISS `tools/wvbank.py`) — then either import in-editor or
bake headless:

```
UnrealEditor-Cmd <project> -run=SuperFAISSUnrealBake -Source=<name>.wvbank.json -Package=/Game/Banks/MyBank
```

The importer validates everything (malformed data is rejected with a line-item error,
never a partial asset), pre-normalizes Cosine banks, quantizes to int8 by default, and
measures the quantization's recall@10 with a recorded seed — the number is stored on
the asset (the demo bank: 0.9916). Channel banks declare `"schemaVersion": 2` with a
`"channels"` array in the sidecar (see the core's FORMAT.md); per-channel recall is
measured and stored per channel.

Validate every bank in CI: `-run=SuperFAISSUnrealValidate` (non-zero exit on any
invalid bank).

Add `-Lint` for on-demand health analyses: near-duplicate rows (sampled above a
configurable cap, never silently exhaustive), low-variance dims, prototype overlap,
and on channel banks: channel-scoped near-duplicates, degenerate channels, and weak
channels (rows whose channel carries almost no energy — their per-channel cosines
are amplified quantization noise; `-ChannelEnergyFloor=` tunes the threshold). In-editor, **Tools > SuperFAISS Bank Inspector** gives live queries with margins,
a PCA projection point cloud of any bank, and — see below — structure/novelty/
correspondence analysis; selected rows become named prototype assets
(`USuperFAISSPrototypeAsset` — also a query provider) via the authoring library.

## Bank Inspector — structure, novelty, and correspondence (v3.2)

![The SuperFAISS Bank Inspector: View A's cluster tree and PCA scatter (an outlier row
selected — one white dot, its amber near-neighbors, the rest of its cluster in the
cluster's own color) beside View B's novelty verdict and evidence list and View C's
per-row correspondence list against a second
bank.](docs/images/BankInspector_3_2.png)

Beyond live queries and the PCA scatter, the Bank Inspector (**Tools > SuperFAISS Bank
Inspector**) answers three questions a raw query can't: *does this bank have natural
clusters, or is it one undifferentiated blob* (View A — Structure), *is this one row
actually new, or a near-duplicate of something already in the bank* (View B —
Novelty), and *which rows in bank A correspond to rows in bank B* (View C —
Correspondence — e.g. a player's saved scratch archive against the shipped reference
bank). All three are built on the core's new `graph.h`/`novelty.h`/`matching.h`
primitives (pure functions over a `BankView`, PER-DEVICE deterministic — layouts and
component/match ids may differ across machines, no cross-device claim).

- **Compute structure** clusters the current sample by mutual k-nearest-neighbor
  agreement plus exact-duplicate grouping, then connected components; the result list
  shows one entry per cluster (+ an Outliers row for anything below the configured
  minimum size), expandable to each member's real bank id. Selecting a cluster
  highlights every one of its points in the scatter; selecting a single row
  highlights three things distinctly — the row itself, its k-nearest neighbors, and
  the rest of its cluster — so a row-level click reads as three claims, not one blob.
- **The novelty probe** (a row id or `#index` typed into the probe box) answers
  duplicate / familiar / novel against the bank's own content: an exact-distance
  identity check first (true 0.0 on Cosine int8, or a byte-identical float32 row, for a real
  duplicate), then a statistical
  rank against a calibrated k-th-neighbor baseline if the identity check doesn't
  already resolve it. The evidence list underneath shows the probe's actual nearest
  neighbors with scores and margins — the verdict's "why," not just its answer.
- **Compute correspondence** needs a second bank (pick one from the second combo, or
  open a scratch archive there — see below); it reports, per sampled row of the
  primary bank, its matched partner in the second bank (or "unmatched"), with a CSLS
  margin and a matched/ambiguous state. This is the disclosed HEAVY pass in the set
  — cost scales with both banks' sizes, not sub-second at scale — the panel discloses
  this before you run it.
- **Open scratch archive…**, beside the normal asset picker on both the primary and
  second-bank slots, loads a saved `USuperFAISSScratchBank` archive file directly (the
  same format `SaveToBytes`/`LoadFromBytes` round-trip) as a transient inspection
  source — no need to bake it to an asset first. A tombstoned (removed) row in an
  archive is honored throughout: it never leaks into a sample, a cluster, or a
  reported correspondence match. Every analysis pass produces the same output over
  the same live rows whether the source is a baked asset or a loaded archive — this
  is the field-debugging use case the archive path exists for.
- **Analysis parameters** (the sample cap, structure's k and minimum cluster size,
  novelty's k and lambda, correspondence's match-k and CSLS threshold) live in one
  per-user, per-project editor settings object and persist across sessions; query
  state (which bank is selected, probe text) stays session-scoped and resets on
  bank change, same as the rest of the Inspector.

### Profiling (v3.2)

Every pass above — and the runtime query path underneath it — is wrapped in a
dedicated, zero-cost-when-off `SuperFAISS` trace channel for Unreal Insights, plus a
`STATGROUP_SuperFAISS` stat group (bytes streamed, effective bandwidth, chunk count,
batch size, per-query time, queries in flight, and the zero-steady-state-allocation
counters). Enable it live in a running editor:

```
Trace.Enable SuperFAISS
Trace.Start
```

...run some queries or Inspector passes, then `Trace.Stop` and open the trace in
Unreal Insights — search the Timing view for `SuperFAISS.*` to find the named spans
(`SuperFAISS.Query`, `SuperFAISS.ScanChunk`, `SuperFAISS.MergeTopK`,
`SuperFAISS.Inspector.StructureBuild`, and so on, one per pass/kernel stage). Or just
watch the live numbers: `stat superfaiss` in any running instance. Instrumentation does
not perturb results in the cases tested — the determinism suite runs trace-OFF and
trace-ON and asserts an identical result on every case in that suite, so a profiling
session has not been observed to change an answer.

## Guarantees

- **Exact**, not approximate: true top-k under Dot, Cosine, or L2.
- **Deterministic per device**: identical bank + query ⇒ bit-identical results,
  regardless of thread count or scheduling (the ranking order is a strict total order;
  serial and parallel paths are test-enforced bit-equal). **Cross-device** exactness
  (bit-identical across different machines) is the opt-in v2.2 mode above.
- **Allocation-free once warm**: pooled workspaces, counted allocations, test-enforced.
- **Thread-safe**: immutable banks, lock-free concurrent reads, 16-thread storm test.
- **Designed for portability**: no platform code, no platform allowlist. SIMD
  (AVX2/SSE4.1/NEON) selected automatically. What is *verified* today: the plugin on
  UE 5.8 Win64 (Editor + automation suite + Shipping game build); the vendored core's
  own CI additionally covers Windows/Linux (AVX2, TSan) and Apple Silicon NEON. Other
  UE targets should work by construction but have not been qualified.
- Scan parallelism is configurable: `superfaiss.ParallelScan` (0 serial, 1 auto,
  2 force) and `superfaiss.ParallelScan.MinChunks`.

## Stripping the demo

The demo module is the plugin's only contact with Mass; nothing outside it
references it. To ship without the demo (or without Mass), three steps:

1. Delete `Source/SuperFAISSUnrealDemo/`.
2. Delete `Content/Demo/`.
3. Remove the `SuperFAISSUnrealDemo` entry from `Modules` in `SuperFAISSUnreal.uplugin`.

The stripped plugin compiles and the non-demo test groups pass unchanged.

## Tests

`SuperFAISS.*` automation tests (113 in this plugin — every registered
`IMPLEMENT_*_AUTOMATION_TEST`, run `Session > Automation` in the editor to see the
current count; 117 with the MCP plugin enabled)
cover kernel correctness, SIMD/scalar mirror equality, determinism, tie-break
stability, concurrency, asset round-trips, import rejection, quantizer recall,
performance guards, query composition (centroid, direction, intersection, margins),
named-channel queries and decomposition, per-row bias (both forms, snapshot
alignment, the decomposition bias term), scratch banks (including the drain gate,
freeze bit-identity, save/load, the recall audit, and report staleness across a
save/load round trip), channel-scoped scratch banks (named-channel scratch queries,
per-channel scratch recall, and the channel vocabulary surviving a save/load round
trip), the runtime-mutable channel vocabulary (`Relabel`: the parity oracle against a
fresh bank, promote/demote, reject-over-degrade, and report staleness), scratch
analytics closures (spread and mean/max nearest-neighbour over a live snapshot),
cross-device pooling (payload and hit-list equality against the core path, and
adversarial payload rejection), the prototype asset's cross-device tier and version
gate, bank lint analyses, prototype authoring, a golden semantic query on the demo
bank, the Mass swarm's stability (F2), the Bank Inspector's structure/novelty/
correspondence panels (including the archive-source path — a tombstoned row proven
absent from every sample, cluster, baseline, and matched pair, asset-vs-archive
output equality, and the second-bank-slot mutual-exclusion/invalidation matrix), and
the instrumentation bar's non-perturbation guarantee (the determinism suite bit-equal
trace-OFF vs. trace-ON) and counter fidelity. Run headless:

```
UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests SuperFAISS; Quit" -unattended -nullrhi
```

`stat superfaiss` shows live query timings.

## The original pitch

v1.0's opening pitch, word for word:

> Fast, deterministic, allocation-free k-nearest-neighbor search over baked embedding
> banks, for Unreal Engine 5.8+. Built on the MIT-licensed
> [SuperFAISS](https://github.com/dansupergameprogrammer/superfaiss) core library
> (vendored under `Source/ThirdParty/SuperFAISS`).

Every word still holds — the plugin just answers more questions now. The banks no
longer have to be baked (scratch banks grow during play and survive save games),
and one bank now serves channel-weighted queries, decomposition, in-scan bias,
prototype categories, and honest per-bank recall numbers alongside the plain top-k
it started with.

## Licensing

- Plugin: MIT (see `LICENSE`). SuperFAISS core: MIT (see
  `Source/ThirdParty/SuperFAISS/LICENSE`).
- Demo bank vectors: GloVe (Pennington, Socher, Manning; Stanford NLP), Public Domain
  Dedication and License v1.0.
