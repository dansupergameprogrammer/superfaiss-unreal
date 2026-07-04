#pragma once

#include "CoreMinimal.h"
#include "Subsystems/EngineSubsystem.h"

#include "SuperFAISSSubsystem.generated.h"

class USuperFAISSVectorBank;
class USuperFAISSScratchBank;

USTRUCT(BlueprintType)
struct FSuperFAISSHit
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Similarity")
	int32 Index = -1;

	UPROPERTY(BlueprintReadOnly, Category = "Similarity")
	FName Id;

	UPROPERTY(BlueprintReadOnly, Category = "Similarity")
	float Score = 0.0f;

	// Score gap to the next-ranked hit, in the scored metric's better-direction —
	// "won by how much". Non-negative; 0 on the last returned hit. The top hit's
	// Margin is the runner-up gap ("best match by how much").
	UPROPERTY(BlueprintReadOnly, Category = "Similarity")
	float Margin = 0.0f;
};

// A named channel with a weight - the Blueprint-sane face of segmented queries
// (plan section 5): names resolve once at query build, never in the kernel.
USTRUCT(BlueprintType)
struct FSuperFAISSChannelWeight
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Similarity")
	FName Channel;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Similarity")
	float Weight = 1.0f;
};

// An explicit element-range segment with a weight (C++ surface; ranges are on the
// bank's 16-byte element grid).
struct FSuperFAISSSegment
{
	int32 Offset = 0;
	int32 Length = 0;
	float Weight = 1.0f;
};

// One sparse per-row bias entry (v2.1): Index is a bank row, Bias adds to that
// row's score in the scored metric's own direction (reward positive on
// Dot/Cosine, NEGATIVE on L2 - lower is better).
USTRUCT(BlueprintType)
struct FSuperFAISSBiasPair
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Similarity")
	int32 Index = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Similarity")
	float Bias = 0.0f;
};

// C++ query options. Blueprint uses the simplified UFUNCTION surface below.
struct FSuperFAISSQueryArgs
{
	int32 K = 10;
	// Optional exclusion bitset, ceil(Count/32) words, bit set = skip row.
	TConstArrayView<uint32> ExcludeBits;
	// Score with the dot kernel regardless of the bank's metric: identity on
	// Dot/Cosine banks; on L2 banks this is the axis-projection path (rank along a
	// MakeDirectionQuery direction). Bank validation rules are unaffected.
	bool bScoreAsDot = false;
	// Segmented query (V2): named channels (resolved against the bank's channel
	// table at query build; on Cosine channel banks the query's channel sub-vectors
	// are renormalized and scores are true per-channel cosines) OR explicit raw
	// ranges. Provide at most one form; empty = the whole row (the V1 path).
	TArray<FSuperFAISSChannelWeight> Channels;
	TArray<FSuperFAISSSegment> Segments;
	// Per-row bias (v2.1): the composed score (similarity + bias) ranks in-scan -
	// exact, unlike post-weighting the returned top-k. At most one form:
	//   RowBias   - dense, exactly Count floats (index-aligned to the bank or, on
	//               scratch queries, to the snapshot the query runs against - a
	//               count mismatch is rejection, never silent misalignment);
	//   BiasPairs - sparse (index, bias) entries, unique in-range indices - the
	//               one-biased-row-per-query shape (motion matching), effectively
	//               free at any scale.
	// Values must be finite (-inf is not a mask; use ExcludeBits). Empty = none,
	// the bit-identical unbiased path. On QueryIntersect the bias applies once, to
	// the fused score. QueryBatch applies the SAME bias to every query in the
	// batch (per-query bias arrays are the core API's RowBias-per-entry surface).
	TArray<float> RowBias;
	TArray<FSuperFAISSBiasPair> BiasPairs;
};

DECLARE_DYNAMIC_DELEGATE_TwoParams(FSuperFAISSResultDelegate,
	const TArray<FSuperFAISSHit>&, Hits, bool, bSuccess);

// Native (C++) completion delegate; the dynamic one above is the Blueprint face.
DECLARE_DELEGATE_TwoParams(FSuperFAISSNativeResultDelegate,
	const TArray<FSuperFAISSHit>& /*Hits*/, bool /*bSuccess*/);

// Handle for an in-flight async query. Cancel() is best-effort: a query that has not
// started computing is dropped; a running scan completes but its delegate reports
// bSuccess=false.
struct SUPERFAISSUNREAL_API FSuperFAISSTicket
{
	TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> CancelFlag;

	void Cancel()
	{
		if (CancelFlag.IsValid())
		{
			CancelFlag->store(true);
		}
	}
	bool IsValid() const { return CancelFlag.IsValid(); }
};

// The runtime query surface over baked banks: synchronous (any thread), asynchronous
// (task graph, bank pinned for the task's lifetime, delegate on the game thread), and
// batched (one bank pass for many queries). Queries are allocation-free once the
// subsystem's workspace pool is warm.
UCLASS()
class SUPERFAISSUNREAL_API USuperFAISSSubsystem : public UEngineSubsystem
{
	GENERATED_BODY()

public:
	// Blocking, callable from any thread. UnpaddedQuery has Bank->Dims elements; the
	// subsystem stages it into aligned padded scratch.
	bool QuerySync(
		const USuperFAISSVectorBank* Bank,
		TConstArrayView<float> UnpaddedQuery,
		const FSuperFAISSQueryArgs& Args,
		TArray<FSuperFAISSHit>& OutHits);

	// Dispatches to the task graph; the bank is pinned against GC until completion.
	// The delegate fires on the game thread with bSuccess=false on cancellation or
	// validation failure.
	FSuperFAISSTicket QueryAsync(
		const USuperFAISSVectorBank* Bank,
		TConstArrayView<float> UnpaddedQuery,
		const FSuperFAISSQueryArgs& Args,
		FSuperFAISSNativeResultDelegate Completion);

	// M queries in one bank pass (queries concatenated, stride Bank->Dims, unpadded).
	// OutHits is query-major, K hits per query (OutCounts holds per-query counts).
	bool QueryBatch(
		const USuperFAISSVectorBank* Bank,
		TConstArrayView<float> UnpaddedQueries,
		int32 QueryCount,
		const FSuperFAISSQueryArgs& Args,
		TArray<FSuperFAISSHit>& OutHits,
		TArray<int32>& OutCounts);

	// Intersection ("similar to ALL of these"): exact top-k over the fused score —
	// each row's worst per-query score in the scored metric's better-direction. Every
	// returned row scores at least the fused score against every member query.
	// Subtraction stays the exclusion bitset; QueryCount == 1 degenerates to
	// QuerySync. One bank pass; queries concatenated, stride Bank->Dims, unpadded.
	bool QueryIntersect(
		const USuperFAISSVectorBank* Bank,
		TConstArrayView<float> UnpaddedQueries,
		int32 QueryCount,
		const FSuperFAISSQueryArgs& Args,
		TArray<FSuperFAISSHit>& OutHits);

	// Scratch-bank query (V2 plan section 7): pins the bank for the flight
	// (V2-G5), snapshots, and scans with the snapshot's tombstones OR'd into the
	// exclusion set - deletion is exclusion. Refused while the bank is draining
	// for a Grow/Freeze/Load (T-044 N4 - this is the one dispatch-point gate).
	// Args.Channels is rejected (scratch banks carry no channel table);
	// Args.Segments raw ranges work. Callable from any thread.
	bool QueryScratch(
		USuperFAISSScratchBank* Bank,
		TConstArrayView<float> UnpaddedQuery,
		const FSuperFAISSQueryArgs& Args,
		TArray<FSuperFAISSHit>& OutHits);

	// Blueprint surface. Sync is intended for small banks; Async for everything else.
	UFUNCTION(BlueprintCallable, Category = "Similarity",
		meta = (DisplayName = "Query Similar (Sync)"))
	bool QuerySimilarSync(const USuperFAISSVectorBank* Bank, const TArray<float>& Query,
		int32 K, TArray<FSuperFAISSHit>& Hits);

	UFUNCTION(BlueprintCallable, Category = "Similarity",
		meta = (DisplayName = "Query Similar (Async)"))
	void QuerySimilarAsync(const USuperFAISSVectorBank* Bank, const TArray<float>& Query,
		int32 K, FSuperFAISSResultDelegate OnComplete);

	// Two-query intersection for Blueprint: rows similar to BOTH queries, ranked by
	// their worse score of the two.
	UFUNCTION(BlueprintCallable, Category = "Similarity",
		meta = (DisplayName = "Query Similar (Intersect)"))
	bool QuerySimilarIntersect(const USuperFAISSVectorBank* Bank,
		const TArray<float>& QueryA, const TArray<float>& QueryB, int32 K,
		TArray<FSuperFAISSHit>& Hits);

	// Named-channel query for Blueprint: rank by a weighted combination of the
	// bank's named channels ("identity 1.0, appearance 0.2"). On Cosine channel
	// banks each channel's score is a true per-channel cosine.
	// Scratch-bank query for Blueprint.
	UFUNCTION(BlueprintCallable, Category = "Similarity",
		meta = (DisplayName = "Query Similar (Scratch)"))
	bool QuerySimilarScratch(USuperFAISSScratchBank* Bank, const TArray<float>& Query,
		int32 K, TArray<FSuperFAISSHit>& Hits);

	UFUNCTION(BlueprintCallable, Category = "Similarity",
		meta = (DisplayName = "Query Similar (Channels)"))
	bool QuerySimilarChannels(const USuperFAISSVectorBank* Bank,
		const TArray<float>& Query, const TArray<FSuperFAISSChannelWeight>& Channels,
		int32 K, TArray<FSuperFAISSHit>& Hits);

	// Decomposition ("why did this match"): per-channel/segment contributions of one
	// row against a segmented query; contributions sum exactly to OutTotal, and
	// OutTotal equals the score the same query's scan produced for that row. Per-hit
	// cost - call it on hits, not banks. RowBias (v2.1) is the bias the caller's
	// query applied to THIS row (0 when unbiased): it reports as the visible
	// separate term - contributions + RowBias = OutTotal, the same single add the
	// scan executed, so the equality stays bitwise.
	UFUNCTION(BlueprintCallable, Category = "Similarity",
		meta = (DisplayName = "Decompose Hit"))
	bool DecomposeHit(const USuperFAISSVectorBank* Bank, const TArray<float>& Query,
		const TArray<FSuperFAISSChannelWeight>& Channels, int32 RowIndex,
		TArray<float>& OutContributions, float& OutTotal, float RowBias = 0.0f);

	// Mean of the selected bank rows as a query vector ("the category's center"):
	// int8 rows dequantize through their per-row scales; on Cosine banks the mean is
	// renormalized, and a zero-norm mean (antipodal members cancelling) fails rather
	// than being renormalized into noise. OutQuery has Bank->Dims elements.
	UFUNCTION(BlueprintCallable, Category = "Similarity",
		meta = (DisplayName = "Make Centroid Query"))
	bool MakeCentroidQuery(const USuperFAISSVectorBank* Bank,
		const TArray<int32>& RowIndices, TArray<float>& OutQuery);

	// Unit direction from B toward A — normalize(A - B) — for "most A-like relative
	// to B" projection queries. A == B fails (no direction). On L2 banks pass the
	// result with bScoreAsDot.
	UFUNCTION(BlueprintCallable, Category = "Similarity",
		meta = (DisplayName = "Make Direction Query"))
	bool MakeDirectionQuery(const TArray<float>& A, const TArray<float>& B,
		TArray<float>& OutQuery);

	// Diagnostics: workspace pool growth count (flat once warm — the B5 counter).
	uint64 GetPoolGrowthCount() const;

	// Drains in-flight async queries. The UObject exit purge runs before the task
	// graph shuts down, so a query in flight at engine exit would read freed bank
	// memory; Deinitialize runs before the purge and waits the fleet out.
	virtual void Deinitialize() override;

	// Pool scratch type; public so the shared RunQuery helper can take it.
	struct FPooledWorkspace;

private:

	TSharedPtr<FPooledWorkspace> AcquireWorkspace();
	void ReleaseWorkspace(TSharedPtr<FPooledWorkspace> Workspace);

	FCriticalSection PoolLock;
	TArray<TSharedPtr<FPooledWorkspace>> Pool;
	std::atomic<uint64> PoolGrowth{0};

	// In-flight async queries (dispatch through game-thread delivery). Drained by
	// Deinitialize; new dispatches during the drain fail immediately.
	std::atomic<int32> InFlightAsync{0};
	std::atomic<bool> bDraining{false};
};
