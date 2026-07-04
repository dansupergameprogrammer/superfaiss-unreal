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

The demo module is the plugin's only contact with Mass and UMG; nothing outside it
references it. To ship without the demo (or without Mass), three steps:

1. Delete `Source/SuperFAISSUnrealDemo/`.
2. Delete `Content/Demo/`.
3. Remove the `SuperFAISSUnrealDemo` entry from `Modules` in `SuperFAISSUnreal.uplugin`.

The stripped plugin compiles and the non-demo test groups pass unchanged.

## Tests

`SuperFAISS.*` automation tests (18) cover kernel correctness, SIMD/scalar mirror
equality, determinism, tie-break stability, concurrency, asset round-trips, import
rejection, quantizer recall, performance guards, and a golden semantic query on the
demo bank. Run headless:

```
UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests SuperFAISS; Quit" -unattended -nullrhi
```

`stat superfaiss` shows live query timings.

## Licensing

- Plugin: MIT (see `LICENSE`). SuperFAISS core: MIT (see
  `Source/ThirdParty/SuperFAISS/LICENSE`).
- Demo bank vectors: GloVe (Pennington, Socher, Manning; Stanford NLP), Public Domain
  Dedication and License v1.0.
