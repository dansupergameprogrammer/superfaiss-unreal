#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "superfaiss/scratch.h"

#include "SuperFAISSVectorBank.h"

#include "SuperFAISSScratchBank.generated.h"

// Recall-audit report (V2.3, plan section 20): the Blueprint-safe mirror of the
// core ScratchRecallReport. The number is reproducible from Seed; Generation
// stamps the bank's mutation state at measurement, so a later Append or newly-set
// Remove renders the report STALE (IsRecallReportStale) — a stale report reads as
// stale, never silently current. bInformative is false below 100 live rows (a
// recall over a tiny personal bank is mathematically valid and statistically
// uninformative, and the report says so); below 11 live rows K is a
// recall@(LiveRows-1) rather than @10, and the K field says so.
USTRUCT(BlueprintType)
struct FSuperFAISSScratchRecallReport
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Similarity|Scratch")
	float Recall = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Similarity|Scratch")
	int32 K = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Similarity|Scratch")
	int32 SampleCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Similarity|Scratch")
	int32 LiveRows = 0;

	// The sampling seed and the generation stamp, bit-cast from the core's uint64.
	UPROPERTY(BlueprintReadOnly, Category = "Similarity|Scratch")
	int64 Seed = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Similarity|Scratch")
	int64 Generation = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Similarity|Scratch")
	bool bInformative = false;
};

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
	// bRetainFloats (V2.3, opt-in, NEVER the default — a dev/audit posture): the
	// arena additionally retains the post-normalization float row beside every
	// quantized row, which is what MeasureRecall audits against. Honest memory
	// cost: an int8 256-dim row grows from 260 to 1284 bytes (~4.9x); the 2x
	// intuition holds only on Float32 banks. Sized into the same single
	// allocation — zero steady-state allocation is preserved.
	UFUNCTION(BlueprintCallable, Category = "Similarity|Scratch")
	bool Init(int32 Capacity, int32 Dims, ESuperFAISSBankMetric Metric,
		ESuperFAISSBankQuantization Quantization, bool bRetainFloats = false);

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

	// The retention flag, as set at Init — a bank property, descriptor-visible
	// (DescribeScratchBank states it).
	UFUNCTION(BlueprintPure, Category = "Similarity|Scratch")
	bool RetainsFloats() const { return Bank.RetainsFloats(); }

	// --- Recall audit (V2.3, plan section 20) ---

	// Seeded recall audit over the current snapshot: the retained float rows scanned
	// in double precision versus the bank's own quantized cross-device scan,
	// recall@k with k = min(10, LiveRows-1), min(1000, LiveRows) self-queries, the
	// self row excluded, tombstoned rows neither sampled nor counted. Uses the
	// core's pinned seed so the number is reproducible (and comparable to a direct
	// core measurement over the same rows). Runs under the reader pin like any
	// query and is refused while the bank drains for a Grow/Freeze/Load (the N4
	// posture). Writer-side: call from one thread, like Append. On a bank Init'd
	// without retention this fails — a defined rejection, never a guessed number.
	// The report is cached for the descriptor surface (GetLastRecallReport).
	UFUNCTION(BlueprintCallable, Category = "Similarity|Scratch")
	bool MeasureRecall(FSuperFAISSScratchRecallReport& OutReport);

	// The last measured report, with its staleness evaluated AT READ against the
	// bank's current generation. False if no report was ever taken.
	UFUNCTION(BlueprintPure, Category = "Similarity|Scratch")
	bool GetLastRecallReport(FSuperFAISSScratchRecallReport& OutReport,
		bool& bOutStale) const;

	// True once any Append or newly-set Remove landed after the report was taken:
	// the report describes rows that no longer exist as measured.
	UFUNCTION(BlueprintPure, Category = "Similarity|Scratch")
	bool IsRecallReportStale(const FSuperFAISSScratchRecallReport& Report) const
	{
		return static_cast<uint64>(Report.Generation) != Bank.Generation();
	}

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

	// Freeze with the V2.3 recall re-measurement threaded through: on a
	// retention-enabled bank the graduated bank carries a FRESH recall number
	// measured over the compacted rows at freeze time (bOutRecallMeasured true;
	// the number equals a direct MeasureRecall of the same rows) — never a
	// pre-mutation report wearing a current face. On a non-retention bank the
	// freeze proceeds and bOutRecallMeasured is false: no number, not a stale one.
	UFUNCTION(BlueprintCallable, Category = "Similarity|Scratch")
	USuperFAISSVectorBank* FreezeWithRecall(TArray<int32>& OutIndexMap,
		FSuperFAISSScratchRecallReport& OutRecallReport, bool& bOutRecallMeasured);

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

	// The shared freeze body; when OutCoreReport is non-null and the bank retains
	// floats, the core re-measures over the compacted rows inside the exclusive
	// window and *bOutMeasured reports it.
	USuperFAISSVectorBank* FreezeInternal(TArray<int32>& OutIndexMap,
		superfaiss::ScratchRecallReport* OutCoreReport, bool* bOutMeasured);

	static void FillReport(const superfaiss::ScratchRecallReport& Core,
		FSuperFAISSScratchRecallReport& Out);

	superfaiss::ScratchBank Bank;
	// Recall-audit scratch (core Workspace: single-owner). MeasureRecall and
	// FreezeWithRecall are writer-side, one at a time, so one workspace serves.
	superfaiss::Workspace RecallWorkspace;
	// The last measured report, cached for the descriptor/MCP surface.
	FSuperFAISSScratchRecallReport LastRecallReport;
	bool bHasRecallReport = false;
};
