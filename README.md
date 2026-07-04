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

![The shipped demo: one query against two GloVe banks, sub-millisecond async results](docs/demo.png)

## Repo layout

| Path | What |
|---|---|
| `SuperFAISSUnreal/` | The plugin. Copy this folder into your project's `Plugins/`. |
| `ExampleProject/` | Minimal host project; opens straight into the demo map. |

## Try it

1. Clone. Open `ExampleProject/ExampleProject.uproject` (UE 5.8+; the project finds
   the plugin in the repo root via `AdditionalPluginDirectories`). Build when prompted.
2. The editor opens on the demo map. Play in editor, type a word at it.

Or headless, from the repo root:

```
UnrealEditor-Cmd ExampleProject/ExampleProject.uproject -ExecCmds="Automation RunTests SuperFAISS; Quit" -unattended -nullrhi
```

18 automation tests: kernel correctness, SIMD/scalar mirror equality, determinism,
tie-break stability, concurrency, asset round-trips, import rejection, quantizer
recall, performance guards, and a golden semantic query on the demo bank.

## Use it in your project

Copy `SuperFAISSUnreal/` into `<YourProject>/Plugins/` and enable it. The plugin is
self-contained (the core library is vendored inside), has no platform allowlist, and
depends only on stable engine modules (the demo module adds Slate/InputCore and is
strippable in three steps) — see the [plugin README](SuperFAISSUnreal/README.md) for
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
