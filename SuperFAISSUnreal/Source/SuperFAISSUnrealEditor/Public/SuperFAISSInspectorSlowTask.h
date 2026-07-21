#pragma once

#include "CoreMinimal.h"

// "Chunked modal slow task, every pass": every
// bank-wide Inspector pass runs on the game thread inside a modal slow-task, chunked
// (per-row-batch progress ticks, cancel checked between chunks). Cancel is a clean abort:
// no partial cache commit, status line reports cancelled. This header is the reusable
// scaffold every sample-scoped M4 pass (Structure, the Novelty baseline) drives through,
// mirroring how graph.h/novelty.h are single synchronous core entries that the WIDGET
// wraps in chunking — the scaffold lives at the wrapping layer, not inside the core
// modules (section 25.4's own note: "chunking and calibrated-constant arithmetic are
// layered on top by the caller").
//
// Design choice this round makes without an explicit plan citation, forced by there being
// no other way to drive deterministic, non-interactive cancellation from an automation
// test (mirrors the M3 test-design round's own three forced-reading choices): the
// cancellation source is caller-injected (`FCancelPoll`), not read directly from
// `FSlowTask::ShouldCancel()` inside this function. Production code's `FCancelPoll` polls
// the real slow task; a test's `FCancelPoll` is a deterministic counter-based lambda. Same
// seam, two callers.
//
// Reviewed as built code: the .cpp body is a real
// `FScopedSlowTask`-backed implementation, driving genuine chunking and cancel.
namespace SuperFAISSInspectorSlowTask
{
	// Returns true to abort the pass at the next chunk boundary. Called once per chunk
	// boundary (including before the first chunk), never mid-chunk — cancel is a
	// clean, chunk-granular abort, never a torn write.
	using FCancelPoll = TFunctionRef<bool()>;

	// One chunk's row range, [ChunkStart, ChunkStart + ChunkCount).
	using FChunkFn = TFunctionRef<void(int32 ChunkStart, int32 ChunkCount)>;

	struct FResult
	{
		// True iff every row in [0, RowCount) was handed to ChunkFn across all chunks.
		bool bCompleted = false;
		// True iff Poll requested abort before RowCount rows were processed. Mutually
		// exclusive with bCompleted (a completed pass was never cancelled, a cancelled
		// pass never completed).
		bool bCancelled = false;
		// The number of rows actually handed to ChunkFn before completion or cancel —
		// RowCount when bCompleted, less than RowCount when bCancelled.
		int32 RowsProcessed = 0;
		// The number of chunk boundaries crossed (== the number of ChunkFn calls made).
		int32 ChunksProcessed = 0;
	};

	// Drives `RowCount` rows through `ChunkFn` in chunks of at most `ChunkSize` rows
	// (`ChunkSize < 1` is treated as `RowCount`, i.e. one chunk — the degenerate,
	// never-cancellable-mid-pass case). `TaskLabel` is the modal slow task's displayed
	// text (production only; a test's `FCancelPoll` does not require a live slow task to
	// exist). `Poll` is checked at every chunk boundary, including before the first
	// chunk (RowCount > 0, Poll() true on the very first check -> RowsProcessed == 0,
	// ChunksProcessed == 0, bCancelled == true, bCompleted == false).
	FResult RunChunked(const FText& TaskLabel, int32 RowCount, int32 ChunkSize,
		FChunkFn ChunkFn, FCancelPoll Poll);
}
