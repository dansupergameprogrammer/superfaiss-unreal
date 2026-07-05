# SuperFAISS For Unreal Engine

Fast, deterministic, allocation-free k-nearest-neighbor search over baked embedding
banks, for Unreal Engine 5.8+. This is the reference engine integration of the
MIT-licensed [SuperFAISS](https://github.com/dansupergameprogrammer/superfaiss) core
library (vendored — no external dependency).

SuperFAISS is an **independent implementation** — not a fork of, derived from, or
affiliated with Meta's FAISS; the name is nominative homage.

Embeddings turn meaning into geometry: words, NPC memories, items, sounds — anything an
encoder model can vectorize — become points where *similar means near*. This plugin
answers "what's most similar to this?" exactly, in microseconds, on any thread.

Measured on the shipped demo bank (40,000 words x 100 dims, int8, ~4 MB), desktop
editor: single query **0.13 ms**, batched **0.06 ms per query** — exact search,
bit-deterministic, zero steady-state allocation.

**New in 2.2:** cross-device bit-exactness — an opt-in query mode (int8 banks)
returning bit-identical scores and hit order on any machine at any SIMD width,
the contract lockstep/rollback multiplayer and networked motion matching
require. The claim runs as a CI test against committed fixtures, it measures
faster than the default int8 scan, and the importer reports the mode's recall
beside standard recall on every bank asset.

**New in 2.1:** per-row score bias, in-scan and exact — sparse (index, bias) pairs
for motion matching's continuing-pose reward (effectively free) and a dense per-row
view for memory salience (+3.5% f32 / +1.9% int8, measured). Finite-only; exclusion
beats bias; rewards are negative on L2.

**New in 2.0:** named channels (rank by a weighted combination of vector sub-spaces,
with exact per-channel decomposition of every hit and true per-channel cosines) and
scratch banks (mutable runtime banks — append/remove/query/freeze/save — for NPC
memory and session-accumulated embeddings). Channels are a semantics feature, not a
speed feature: a channel query costs approximately one full scan, and the docs say
so plainly. See the [plugin README](SuperFAISSUnreal/README.md).

![The shipped demo: one query against two GloVe banks, sub-millisecond async results](docs/demo.png)

## Repo layout

| Path | What |
|---|---|
| `SuperFAISSUnreal/` | The plugin. Copy this folder into your project's `Plugins/`. |
| `SuperFAISSUnrealMCP/` | Optional agent toolset: MCP tools over your banks. Requires Experimental engine plugins most distributions don't carry — see its README. Disabled by default; everything else works without it. |
| `ExampleProject/` | Minimal host project; opens straight into the demo map. |

## Try it

1. Clone. Open `ExampleProject/ExampleProject.uproject` (UE 5.8+; the project finds
   the plugin in the repo root via `AdditionalPluginDirectories`). Build when prompted.
2. The editor opens on the demo map. Play in editor, type a word at it.

Or headless, from the repo root:

```
UnrealEditor-Cmd ExampleProject/ExampleProject.uproject -ExecCmds="Automation RunTests SuperFAISS; Quit" -unattended -nullrhi
```

31 automation tests (33 with the optional MCP toolset enabled): kernel correctness,
SIMD/scalar mirror equality, determinism, tie-break stability, concurrency, asset
round-trips, import rejection, quantizer recall, performance guards, query
composition (centroid, direction, intersection, margins), named-channel queries and
decomposition, per-row bias, scratch banks, cross-device exactness (v2.2 — including
a golden-hash battery over committed fixtures that must match the core CI's pin),
bank lint analyses, prototype authoring, a golden semantic query on the demo bank,
and the Mass swarm's stability.

## Use it in your project

Copy `SuperFAISSUnreal/` into `<YourProject>/Plugins/` and enable it. The plugin is
self-contained (the core library is vendored inside), has no platform allowlist, and
depends only on stable engine modules (the demo module adds Slate/InputCore and
Mass — UE 5.8 engine modules, no plugin references — and is strippable in three
steps) — see the [plugin README](SuperFAISSUnreal/README.md) for
the API quick start, bank authoring, guarantees, and demo-strip steps.

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

## Licensing

- Plugin and example project: MIT (see `LICENSE`).
- SuperFAISS core: MIT (`SuperFAISSUnreal/Source/ThirdParty/SuperFAISS/LICENSE`).
- Demo bank vectors: GloVe (Pennington, Socher, Manning; Stanford NLP), Public Domain
  Dedication and License v1.0.
