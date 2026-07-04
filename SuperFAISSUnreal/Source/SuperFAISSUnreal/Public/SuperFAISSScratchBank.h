#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "superfaiss/scratch.h"

#include "SuperFAISSVectorBank.h"

#include "SuperFAISSScratchBank.generated.h"

// A mutable, append-friendly bank (V2 plan section 7) — runtime state, not an
// asset: NPC memory, session-accumulated embeddings, anything that grows during
// play. Wraps the core superfaiss::ScratchBank; queries dispatch through
// USuperFAISSSubsystem::QueryScratch over a snapshot view, so every query
// feature (segments, exclusion, ScoreAs) works on scratch content unchanged.
//
// Concurrency contract: ONE logical writer (Append/Remove/Grow/Freeze/Load are
// writer-side; call them from one thread — in practice the game thread).
// Queries are lock-free readers and may run from any thread; they pin the bank
// for their flight (V2-G5). Grow/Freeze/Load drain: they refuse while waiting
// on in-flight queries, and the subsystem refuses NEW queries while they wait
// (T-044 N4) — without that refusal a busy consumer starves the operation.
//
// Index stability (T-044 W4): Grow preserves row indices — a stored index means
// the same row before and after. Freeze compacts (drops removed rows) and
// renumbers; it returns the old->new map so stored handles can be remapped as
// part of the freeze.
UCLASS(BlueprintType)
class SUPERFAISSUNREAL_API USuperFAISSScratchBank : public UObject
{
	GENERATED_BODY()

public:
	// Allocates the arena (one allocation; zero steady-state allocation after).
	// Capacity is the memory budget made explicit. Fails on a re-init.
	UFUNCTION(BlueprintCallable, Category = "Similarity|Scratch")
	bool Init(int32 Capacity, int32 Dims, ESuperFAISSBankMetric Metric,
		ESuperFAISSBankQuantization Quantization);

	// Validates like the importer (finite values, dims match); Cosine banks
	// normalize on append and reject zero-norm rows; int8 banks quantize the row
	// standalone. Returns false with the capacity exhausted — Grow to proceed.
	UFUNCTION(BlueprintCallable, Category = "Similarity|Scratch")
	bool Append(const TArray<float>& Vector, int32& OutIndex);

	// Tombstones the row. Removal is snapshot-consistent: queries already in
	// flight may still return it; every later query excludes it. Idempotent.
	UFUNCTION(BlueprintCallable, Category = "Similarity|Scratch")
	bool Remove(int32 Index);

	UFUNCTION(BlueprintPure, Category = "Similarity|Scratch")
	int32 GetCount() const { return Bank.Count(); }

	UFUNCTION(BlueprintPure, Category = "Similarity|Scratch")
	int32 GetLiveCount() const { return Bank.LiveCount(); }

	UFUNCTION(BlueprintPure, Category = "Similarity|Scratch")
	int32 GetCapacity() const { return Bank.Capacity(); }

	UFUNCTION(BlueprintPure, Category = "Similarity|Scratch")
	int32 GetDims() const { return Bank.Dims(); }

	UFUNCTION(BlueprintPure, Category = "Similarity|Scratch")
	ESuperFAISSBankMetric GetMetric() const
	{
		return static_cast<ESuperFAISSBankMetric>(Bank.GetMetric());
	}

	UFUNCTION(BlueprintPure, Category = "Similarity|Scratch")
	ESuperFAISSBankQuantization GetQuantization() const
	{
		return static_cast<ESuperFAISSBankQuantization>(Bank.GetQuantization());
	}

	UFUNCTION(BlueprintPure, Category = "Similarity|Scratch")
	bool IsInitialized() const { return Bank.IsCreated(); }

	// Index-preserving reallocation (T-044 W4). Drains in-flight queries first;
	// new queries are refused while it waits (N4).
	UFUNCTION(BlueprintCallable, Category = "Similarity|Scratch")
	bool Grow(int32 NewCapacity);

	// Graduates accumulated memory to an immutable bank: compacts live rows into
	// a new USuperFAISSVectorBank (runtime bake — the payload is copied, not
	// re-quantized, so the frozen bank scores bit-identically to this one).
	// OutIndexMap has GetCount() entries: old index -> compacted index, -1 for
	// removed rows — remap stored handles through it as part of the freeze.
	// Drains like Grow. Returns null on failure.
	UFUNCTION(BlueprintCallable, Category = "Similarity|Scratch")
	USuperFAISSVectorBank* Freeze(TArray<int32>& OutIndexMap);

	// Full-state persistence (save games): header, rows, scales, tombstones.
	// Save is writer-side (readers keep flying); Load drains and replaces —
	// reject-over-degrade, a bad blob leaves the bank unchanged.
	UFUNCTION(BlueprintCallable, Category = "Similarity|Scratch")
	bool SaveToBytes(TArray<uint8>& OutBytes) const;

	UFUNCTION(BlueprintCallable, Category = "Similarity|Scratch")
	bool LoadFromBytes(const TArray<uint8>& Bytes);

	// --- C++ query-pin seam (used by USuperFAISSSubsystem::QueryScratch) ---

	// The N4 dispatch gate: fails while a drain-requiring operation is waiting,
	// otherwise pins the bank for one query flight. Delegates to the core
	// pin/drain primitive, whose seq_cst orderings close the store-buffering
	// window on weakly-ordered ISAs (Poirot F4) and which runs under
	// ThreadSanitizer in core CI - the shipped protocol is the verified one.
	bool TryPin() { return Bank.TryPinReader(); }
	void Unpin() { Bank.UnpinReader(); }

	superfaiss::ScratchBank& Core() { return Bank; }
	const superfaiss::ScratchBank& Core() const { return Bank; }

private:
	// Refuses new pins, waits out in-flight pins (core BeginExclusive), runs Op,
	// releases. Writer-side; the wait is bounded by query flights, which are
	// sub-millisecond at game scale.
	bool DrainAndRun(TFunctionRef<bool()> Op);

	superfaiss::ScratchBank Bank;
};
