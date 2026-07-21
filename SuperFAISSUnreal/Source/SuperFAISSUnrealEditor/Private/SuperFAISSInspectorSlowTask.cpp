#include "SuperFAISSInspectorSlowTask.h"

#include "Math/UnrealMathUtility.h"
#include "Misc/ScopedSlowTask.h"

namespace SuperFAISSInspectorSlowTask
{
	FResult RunChunked(const FText& TaskLabel, int32 RowCount, int32 ChunkSize,
		FChunkFn ChunkFn, FCancelPoll Poll)
	{
		FResult Result;
		if (RowCount <= 0)
		{
			Result.bCompleted = true;
			return Result;
		}

		// ChunkSize < 1 degenerates to one chunk covering the whole range (header contract).
		const int32 EffectiveChunkSize = ChunkSize < 1 ? RowCount : ChunkSize;
		const int32 ChunkCountEstimate =
			(RowCount + EffectiveChunkSize - 1) / EffectiveChunkSize;

		FScopedSlowTask SlowTask(static_cast<float>(ChunkCountEstimate), TaskLabel);
		SlowTask.MakeDialog(/*bShowCancelButton*/ true);

		// Poll is checked at every chunk boundary, including before the first chunk — a
		// cancel on the very first check aborts before any row is handed to ChunkFn.
		// Checked alongside SlowTask.ShouldCancel(): the modal dialog's own Cancel button
		// is the PRODUCTION cancel source, and it lives on the FScopedSlowTask this
		// function owns internally, invisible to the caller's injected Poll -- a caller
		// with no other cancel source of its own passes a trivial `[]{ return false; }`
		// and relies entirely on the dialog. A test's deterministic Poll (no live dialog,
		// ShouldCancel() always false there) is unaffected -- the OR degrades to Poll()
		// alone exactly as before.
		while (Result.RowsProcessed < RowCount)
		{
			if (Poll() || SlowTask.ShouldCancel())
			{
				Result.bCancelled = true;
				return Result;
			}
			const int32 ThisChunkCount =
				FMath::Min(EffectiveChunkSize, RowCount - Result.RowsProcessed);
			ChunkFn(Result.RowsProcessed, ThisChunkCount);
			Result.RowsProcessed += ThisChunkCount;
			++Result.ChunksProcessed;
			SlowTask.EnterProgressFrame(1.0f);
		}
		Result.bCompleted = true;
		return Result;
	}
}
