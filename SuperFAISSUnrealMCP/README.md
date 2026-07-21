# SuperFAISS MCP Toolset

MCP tools over SuperFAISS vector banks: exposes enumerate, describe, query, import,
validate, and lint operations over a running editor's MCP server. Ships as its own
plugin beside `SuperFAISSUnreal`, disabled by default.

## Requirements — read this first

This plugin depends on **Experimental, `NoRedist` engine plugins** that stock engine
distributions are expected not to include (expected, not verified — unconfirmable from
a source tree):

- `ToolsetRegistry` — the tool-authoring surface this plugin registers with (hard
  dependency).
- `ModelContextProtocol` — the MCP server that exposes registered toolsets to clients
  (used at runtime, never depended on directly).

In practice that means **a GitHub-source engine build**. Everything else in the
SuperFAISS plugins works everywhere; this one stays disabled unless your engine can
host it. No engine code is copied here — dependency only.

## Enabling

1. Confirm your engine tree has `Engine/Plugins/Experimental/ToolsetRegistry` and
   `Engine/Plugins/Experimental/ModelContextProtocol`.
2. Enable `SuperFAISSUnrealMCP` and `ModelContextProtocol` in your project.
3. Start the editor; the MCP server listens (default port 8000). The toolset registers
   as `SuperFAISSUnrealMCP.SuperFAISSToolset` — discover it through the server's
   `list_toolsets` / `describe_toolset` / `call_tool` meta-tools.

Expect Experimental-grade API churn from the engine side; this plugin is one class of
thin wrappers and tracks such churn cheaply.

## Tools

| Tool | What it does |
|---|---|
| `Echo` | Connectivity probe; names the executing thread |
| `ListBanks` | Every bank in the project: path, count, dims, metric, quantization, size |
| `DescribeBank` | Full metadata for one bank, including recall@10, cross-device recall@10 (v2.2), and source hash |
| `QueryBank` | Exact top-K by row id, row index, or raw vector; hits carry scores and margins. On channel banks (schemaVersion 2), `channelNames` + `channelWeights` rank by a weighted combination of named channels. `biasIndices` + `biasValues` (v2.1) add a per-row score bias in-scan — the composed ranking is exact; sparse pairs are the only bias form MCP exposes. `crossDeviceExact` (v2.2) runs the cross-device exactness mode: bit-identical scores and hit order on any machine at any SIMD width; int8 banks only |
| `QueryPrototype` | Top-K against a centroid of listed rows, or a saved prototype asset |
| `ProjectAxis` | Rank the bank along `normalize(A−B)` — "most A-like relative to B" |
| `ImportBank` | Sidecar pair → validated bank asset with a seeded recall report; destination confined to `/Game`; existing assets refused unless overwrite is explicit; can take seconds on large banks |
| `ValidateBanks` | Project-wide bank validation with per-bank diagnostics |
| `LintBank` | Near-duplicate rows (sampled above a cap, and the report says so) and low-variance dims; on channel banks also channel-scoped near-duplicates, degenerate channels, and weak channels; cost scales with bank size |
| `ListScratchBanks` | Live scratch banks (runtime, mutable, in-memory — not assets); read-only |
| `DescribeScratchBank` | Metadata for one live scratch bank, by the object path `ListScratchBanks` reported; states the v2.3 float-retention flag and, once game code measured one, the recall-audit report with its generation stamp and stale mark |
| `QueryScratchBank` | Exact top-K against a live scratch bank by raw vector; removed rows excluded automatically |

Read/import-only by design: no tool deletes or mutates an existing bank, and scratch
banks are never mutated through MCP — game code owns their writes. There is no
text encoder — an id resolves to a row or it does not resolve; anything that turns
domain data into vectors lives on your side of the encoder seam (see the
[`SuperFAISSUnreal` README](../SuperFAISSUnreal/README.md)).

## Licensing

MIT (see `LICENSE`). Contains no engine code.
