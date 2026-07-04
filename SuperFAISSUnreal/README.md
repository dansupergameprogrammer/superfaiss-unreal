# SuperFAISSUnreal

Fast, deterministic, allocation-free k-nearest-neighbor search over baked embedding
banks, for Unreal Engine 5.8+. Built on the MIT-licensed
[SuperFAISS](https://github.com/dansupergameprogrammer/superfaiss) core library
(vendored under `Source/ThirdParty/SuperFAISS`).

SuperFAISS is an **independent implementation** — not a fork of, derived from, or
affiliated with Meta's FAISS; the name is nominative homage.

Embeddings turn meaning into geometry: words, NPC memories, items, sounds — anything an
encoder model can vectorize — become points where *similar means near*. This plugin
answers "what's most similar to this?" exactly, in microseconds, on any thread.

Measured on the shipped demo bank (40,000 words × 100 dims, int8, ~4 MB), desktop editor:
single query **0.13 ms**, batched **0.06 ms per query** — exact search, bit-deterministic,
zero steady-state allocation.

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
the asset (the demo bank: 0.9916).

Validate every bank in CI: `-run=SuperFAISSUnrealValidate` (non-zero exit on any
invalid bank).

Add `-Lint` for on-demand health analyses: near-duplicate rows (sampled above a
configurable cap, never silently exhaustive), low-variance dims, and prototype
overlap. In-editor, **Tools > SuperFAISS Bank Inspector** gives live queries with
margins and a PCA projection point cloud of any bank; selected rows become named
prototype assets (`USuperFAISSPrototypeAsset` — also a query provider) via the
authoring library.

## Guarantees

- **Exact**, not approximate: true top-k under Dot, Cosine, or L2.
- **Deterministic per device**: identical bank + query ⇒ bit-identical results,
  regardless of thread count or scheduling (the ranking order is a strict total order;
  serial and parallel paths are test-enforced bit-equal).
- **Allocation-free once warm**: pooled workspaces, counted allocations, test-enforced.
- **Thread-safe**: immutable banks, lock-free concurrent reads, 16-thread storm test.
- **Every platform**: no platform code, no platform allowlist. SIMD (AVX2/SSE4.1/NEON)
  selected automatically; NEON is CI-verified on Apple Silicon.
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

`SuperFAISS.*` automation tests (26) cover kernel correctness, SIMD/scalar mirror
equality, determinism, tie-break stability, concurrency, asset round-trips, import
rejection, quantizer recall, performance guards, query composition (centroid,
direction, intersection, margins), bank lint analyses, prototype authoring, a golden
semantic query on the demo bank, and the Mass swarm's stability (F2). Run headless:

```
UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests SuperFAISS; Quit" -unattended -nullrhi
```

`stat superfaiss` shows live query timings.

## Licensing

- Plugin: MIT (see `LICENSE`). SuperFAISS core: MIT (see
  `Source/ThirdParty/SuperFAISS/LICENSE`).
- Demo bank vectors: GloVe (Pennington, Socher, Manning; Stanford NLP), Public Domain
  Dedication and License v1.0.
