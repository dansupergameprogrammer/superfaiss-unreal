#pragma once

#include "CoreMinimal.h"
#include "Subsystems/EngineSubsystem.h"

#include "SuperFAISSSubsystem.generated.h"

class USuperFAISSVectorBank;
class USuperFAISSScratchBank;

// Reduction kind for the V2.5 divergence/spread reductions (plan section 22) — the
// Blueprint-facing mirror of core superfaiss::Reduce (ordinals shared): Mean is a
// fixed-order ascending-index double accumulation, Max an order-free running max.
UENUM()
enum class ESuperFAISSReduce : uint8
{
	Mean = 0,
	Max = 1,
};

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

// A pooled cross-device query payload (v2.4, plan section 21): the quantized
// centroid MakeCentroidQueryCrossDevice produced — the int8 image (stored as its
// byte pattern), the per-query dequant scale (double, no float round-trip), and
// the integer self-dot. QueryPooledCrossDevice executes EXACTLY these bytes (the
// core QueryXd path): no float round-trip, no requantization, so the executed
// query is bit-for-bit the operator's product on every machine. Dims/PaddedDims
// bind the payload to its bank shape.
USTRUCT(BlueprintType)
struct FSuperFAISSCrossDeviceQuery
{
	GENERATED_BODY()

	// int8 image bytes on the bank's padded grid (PaddedDims entries).
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Similarity")
	TArray<uint8> ImageQ8;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Similarity")
	double Scale = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Similarity")
	int64 SqSum = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Similarity")
	int32 Dims = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Similarity")
	int32 PaddedDims = 0;

	// Payload integrity (review S2/M1, the trust-boundary class): a hand-edited or
	// corrupted payload (a Blueprint-authored struct, a desynced prototype asset)
	// must be a defined rejection, never a silently wrong ranking. The scale must
	// be FINITE and non-negative (a bare >= 0 admits +inf, which poisons Dot/L2
	// scores with NaN), and the self-dot must be the image's own (it feeds the L2
	// epilogue; a lying value corrupts rankings). O(PaddedDims) — noise beside the
	// scan it gates; the core boundary re-validates identically.
	bool IsPayloadValid() const
	{
		if (ImageQ8.Num() <= 0 || ImageQ8.Num() != PaddedDims || Dims <= 0 ||
			Dims > PaddedDims || !(Scale >= 0.0) || !FMath::IsFinite(Scale) || SqSum < 0)
		{
			return false;
		}
		int64 Sq = 0;
		for (const uint8 Byte : ImageQ8)
		{
			const int64 V = static_cast<int8>(Byte); // stored bit pattern is int8
			Sq += V * V;
		}
		return Sq == SqSum;
	}
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
	// Cross-device exactness (v2.2): opt this query into bit-identical scores AND
	// hit order across DIFFERENT machines and SIMD widths (x86/ARM, any OS) - the
	// lockstep / rollback / networked-motion-matching contract. Int8 banks only
	// (an f32 bank fails the query); composes with everything above. The query is
	// quantized to int8 and scored through integer accumulation with a fixed-order
	// double epilogue; any final score with magnitude below FLT_MIN is exactly
	// 0.0f, by contract (the FTZ/DAZ hole, closed by specification). Query
	// quantization adds error beyond row quantization - the importer measures and
	// stores cross-device recall beside standard recall; check it before adopting.
	// Cross-device scores are not equal to default-mode scores (different math).
	// Full contract: vendored DETERMINISM.md section 2c.
	bool bCrossDeviceExact = false;
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
	// Args.Channels/Segments apply to every query in the batch; Args.RowBias/
	// BiasPairs apply the SAME bias to every query. PerQueryBiasPairs (empty,
	// or exactly QueryCount entries) instead gives each query its OWN sparse
	// pair - the crowd shape (one continuing-pose reward per entity, the
	// core v2.1 sparse form's named consumer); entries with Index == INDEX_NONE
	// are unbiased. PerQueryBiasPairs is exclusive with the Args bias forms.
	// Composed batches (segments, any bias, or cross-device) run the serial
	// core batch; the parallel chunk fan-out stays the plain-query fast path.
	bool QueryBatch(
		const USuperFAISSVectorBank* Bank,
		TConstArrayView<float> UnpaddedQueries,
		int32 QueryCount,
		const FSuperFAISSQueryArgs& Args,
		TArray<FSuperFAISSHit>& OutHits,
		TArray<int32>& OutCounts,
		TConstArrayView<FSuperFAISSBiasPair> PerQueryBiasPairs = {});

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

	// Named-channel query against a live scratch bank (V3.0 slot 5): the scratch
	// sibling of QuerySimilarChannels. The bank must carry a channel table
	// (InitWithChannels); channels resolve against the scratch bank's own vocabulary.
	// On Cosine channel scratch banks each channel's score is a true per-channel
	// cosine. Removed rows are excluded automatically; refused while the bank drains.
	UFUNCTION(BlueprintCallable, Category = "Similarity|Scratch",
		meta = (DisplayName = "Query Similar (Scratch, Channels)"))
	bool QuerySimilarChannelsScratch(USuperFAISSScratchBank* Bank,
		const TArray<float>& Query, const TArray<FSuperFAISSChannelWeight>& Channels,
		int32 K, TArray<FSuperFAISSHit>& Hits);

	// Cross-device exact query for Blueprint (v2.2): bit-identical scores and hit
	// order on any machine at any SIMD width. Int8 banks only; fails on f32 banks.
	UFUNCTION(BlueprintCallable, Category = "Similarity",
		meta = (DisplayName = "Query Similar (Cross-Device Exact)"))
	bool QuerySimilarCrossDevice(const USuperFAISSVectorBank* Bank,
		const TArray<float>& Query, int32 K, TArray<FSuperFAISSHit>& Hits);

	// Decomposition ("why did this match"): per-channel/segment contributions of one
	// row against a segmented query; contributions sum exactly to OutTotal, and
	// OutTotal equals the score the same query's scan produced for that row. Per-hit
	// cost - call it on hits, not banks. RowBias (v2.1) is the bias the caller's
	// query applied to THIS row (0 when unbiased): it reports as the visible
	// separate term - contributions + RowBias = OutTotal, the same single add the
	// scan executed, so the equality stays bitwise.
	// bCrossDeviceExact (v2.2) decomposes through the cross-device kernel instead,
	// matching a bCrossDeviceExact query's scan score bitwise; in that mode each
	// contribution carries the subnormal floor and OutTotal is the scan's double
	// chain converted once - NOT the float re-sum of the contributions (a
	// default-mode property only).
	UFUNCTION(BlueprintCallable, Category = "Similarity",
		meta = (DisplayName = "Decompose Hit"))
	bool DecomposeHit(const USuperFAISSVectorBank* Bank, const TArray<float>& Query,
		const TArray<FSuperFAISSChannelWeight>& Channels, int32 RowIndex,
		TArray<float>& OutContributions, float& OutTotal, float RowBias = 0.0f,
		bool bCrossDeviceExact = false);

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

	// Integer-domain pooling (v2.4, plan section 21): pools the selected int8 bank
	// rows into a QUANTIZED cross-device query — order-free int64 accumulation with
	// a direct integer-domain requantization (no float mean, no norm reduction), so
	// the payload is bit-identical on every machine given the same rows. Weights
	// (optional salience, empty = unweighted; else one positive integer per row;
	// all-equal weights produce the bit-identical unweighted payload) fold into the
	// same integer multiply. int8 banks only (the float MakeCentroidQuery remains
	// the presentation-tier path); a selection whose integer accumulator cancels to
	// zero fails, like the float path's zero-norm centroid. The pooled sum of
	// weights is capped at 2^20 (the core's overflow-proof bound). Recall cost is
	// measured beside the operator (core suite: ~0.99 recall@10 on random int8 at
	// the first calibration); this operator is space-version-bound for consumers
	// (vendored DETERMINISM.md section 2d).
	UFUNCTION(BlueprintCallable, Category = "Similarity",
		meta = (DisplayName = "Make Centroid Query (Cross-Device Exact)"))
	bool MakeCentroidQueryCrossDevice(const USuperFAISSVectorBank* Bank,
		const TArray<int32>& RowIndices, const TArray<int32>& Weights,
		FSuperFAISSCrossDeviceQuery& OutQuery);

	// Executes a pooled cross-device payload against its bank — the core QueryXd
	// path: the same integer kernels, fixed-order double epilogue, and subnormal
	// floor a bCrossDeviceExact query runs, on the payload's exact bytes. The
	// payload must match the bank's shape (Dims/PaddedDims); int8 banks only.
	UFUNCTION(BlueprintCallable, Category = "Similarity",
		meta = (DisplayName = "Query Similar (Pooled Cross-Device)"))
	bool QueryPooledCrossDevice(const USuperFAISSVectorBank* Bank,
		const FSuperFAISSCrossDeviceQuery& Query, int32 K, TArray<FSuperFAISSHit>& Hits);

	// --- V2.5 bank analytics (plan section 22) ---
	// Cross-device deterministic reductions over int8 banks — set-to-set distance,
	// directed nearest-neighbour divergence, and within-bank dispersion — plus the
	// shared query-vs-query pair score and the per-device projection report. Each BP
	// method wraps the vendored core operator: the UE-compiled core is the same code,
	// so the returned scalar is bit-identical to a direct core call on the same rows
	// (the cross-device contract; DETERMINISM.md section 2c/2d). Int8 cross-device
	// banks only; an f32 or mismatched bank is the mapped rejection (false), never a
	// silently wrong number. The subsystem owns the scratch/workspace so callers
	// allocate nothing once the pool is warm.

	// Set-to-set centroid distance (plan 22.4): pools each row selection cross-device
	// (MakeCentroidQueryCrossDevice) and scores the pair in Metric's distance sense.
	// DRIFT over checkpoints is this method between two checkpoints' banks and row
	// sets — no separate method. Weights are optional salience (empty = unweighted;
	// else one positive weight per row). Empty selection -> false; a zero-norm pooled
	// accumulator -> false (the cosine-antipodal case). Both banks int8 cross-device
	// with equal Dims/PaddedDims.
	UFUNCTION(BlueprintCallable, Category = "Similarity|Analytics",
		meta = (DisplayName = "Set-To-Set Distance (Cross-Device)"))
	bool SetToSetDistanceCrossDevice(const USuperFAISSVectorBank* BankA,
		const TArray<int32>& RowIndicesA, const TArray<int32>& WeightsA,
		const USuperFAISSVectorBank* BankB, const TArray<int32>& RowIndicesB,
		const TArray<int32>& WeightsB, ESuperFAISSBankMetric Metric, float& OutDistance);

	// Directed mean nearest-neighbour set divergence (plan 22.4): for each live row of
	// SourceBank, its nearest-neighbour distance to TargetBank, reduced by a
	// fixed-order mean (ascending source-row index). Scored in the TARGET bank's own
	// metric (Cosine -> 1-cos, L2 -> squared distance, Dot -> similarity). Directed
	// A->B; a symmetric divergence is the caller's reduce of both directions. Both
	// banks int8 cross-device with equal shape.
	UFUNCTION(BlueprintCallable, Category = "Similarity|Analytics",
		meta = (DisplayName = "Mean Nearest Neighbor (Cross-Device)"))
	bool MeanNearestNeighborCrossDevice(const USuperFAISSVectorBank* SourceBank,
		const USuperFAISSVectorBank* TargetBank, float& OutValue);

	// Directed max nearest-neighbour set divergence (plan 22.4): the same directed
	// nearest-neighbour set as the mean, reduced by an order-free max — the directed
	// Hausdorff component.
	UFUNCTION(BlueprintCallable, Category = "Similarity|Analytics",
		meta = (DisplayName = "Max Nearest Neighbor (Cross-Device)"))
	bool MaxNearestNeighborCrossDevice(const USuperFAISSVectorBank* SourceBank,
		const USuperFAISSVectorBank* TargetBank, float& OutValue);

	// Within-bank dispersion (plan 22.4, spread = centroid-dispersion, D-V2-11): the
	// mean (or max) distance of each selected row to the selection's OWN cross-device
	// centroid, in the bank's metric. RowIndices are the rows to include (ascending
	// for the pinned mean order); empty selection -> false.
	UFUNCTION(BlueprintCallable, Category = "Similarity|Analytics",
		meta = (DisplayName = "Bank Spread (Cross-Device)"))
	bool BankSpreadCrossDevice(const USuperFAISSVectorBank* Bank,
		const TArray<int32>& RowIndices, ESuperFAISSReduce Reduce, float& OutValue);

	// Cross-device score between two pooled/lifted payloads (plan 22.4): the shared
	// primitive the operators above rest on, in Metric's distance sense (Dot: dot
	// similarity; L2: squared distance; Cosine: 1 - cos, the one runtime sqrt). This
	// is the PUBLIC boundary and carries the D-V2-13 guard: it validates both payloads
	// (IsPayloadValid) and rejects any int8 image element equal to -128 (the +-127
	// premise the int32 cross-dot bound rests on) -> false. Cosine requires a nonzero
	// self-dot on both members -> false otherwise.
	UFUNCTION(BlueprintCallable, Category = "Similarity|Analytics",
		meta = (DisplayName = "Score Cross-Device Query Pair"))
	bool ScoreCrossDeviceQueryPair(const FSuperFAISSCrossDeviceQuery& A,
		const FSuperFAISSCrossDeviceQuery& B, ESuperFAISSBankMetric Metric,
		float& OutScore);

	// Feature A — the probe-direction projection report (plan 22.3). OFFLINE authoring
	// tooling, per-device float, NO cross-device claim: projects each bank row onto the
	// unit direction normalize(DirectionA - DirectionB) (built like MakeDirectionQuery
	// and extended to the full row set), writing OutProjections (Bank->Count floats).
	// When GroupA is non-empty, OutSeparation is the Cohen's-d separation of GroupA's
	// projected mean against the remaining rows' — the report FINDS the separation it
	// exists to find. A zero-norm/degenerate direction, an empty bank, or a GroupA that
	// covers every row (empty complement) -> false. DirectionA/B have Bank->Dims
	// elements.
	UFUNCTION(BlueprintCallable, Category = "Similarity|Analytics",
		meta = (DisplayName = "Projection Report"))
	bool ProjectionReport(const USuperFAISSVectorBank* Bank,
		const TArray<float>& DirectionA, const TArray<float>& DirectionB,
		const TArray<int32>& GroupA, TArray<float>& OutProjections,
		float& OutSeparation);

	// --- V2.5 analytics over a live scratch snapshot (plan section 22, T-V2.5-U2) ---
	// Scratch overloads mirror QueryScratch: pin the bank, snapshot to a BankView, OR
	// the snapshot's tombstones into the analytics exclusion set (deletion is
	// exclusion), run the same core operator over the snapshot, unpin. The scalar is
	// the core snapshot result — a tombstoned row contributes to neither the pool nor
	// the reduction. The scratch operand is the natural single/source/A side; the
	// counterpart is a baked bank (the "drift from a reference bank" shape). Refused
	// while the bank drains for a Grow/Freeze/Load (the N4 gate). Callable from any
	// thread.

	// Within-scratch-snapshot dispersion over the live rows (the scratch closure of
	// BankSpreadCrossDevice).
	UFUNCTION(BlueprintCallable, Category = "Similarity|Analytics",
		meta = (DisplayName = "Bank Spread (Cross-Device, Scratch)"))
	bool BankSpreadCrossDeviceScratch(USuperFAISSScratchBank* Bank,
		ESuperFAISSReduce Reduce, float& OutValue);

	// Directed mean nearest-neighbour divergence from a scratch snapshot's live rows to
	// a baked TargetBank (the scratch closure of MeanNearestNeighborCrossDevice).
	UFUNCTION(BlueprintCallable, Category = "Similarity|Analytics",
		meta = (DisplayName = "Mean Nearest Neighbor (Cross-Device, Scratch Source)"))
	bool MeanNearestNeighborCrossDeviceScratch(USuperFAISSScratchBank* SourceBank,
		const USuperFAISSVectorBank* TargetBank, float& OutValue);

	// Directed max nearest-neighbour divergence from a scratch snapshot's live rows to a
	// baked TargetBank — the scratch closure of MaxNearestNeighborCrossDevice, and the
	// order-free (max) sibling of MeanNearestNeighborCrossDeviceScratch. Added so the
	// read-only MCP SetToSetDistance tool's maxNN mode has a scratch path (V3-G7 / §23.3
	// Forge W1); the core MaxNNCrossDevice reduction is shared with the baked overload.
	UFUNCTION(BlueprintCallable, Category = "Similarity|Analytics",
		meta = (DisplayName = "Max Nearest Neighbor (Cross-Device, Scratch Source)"))
	bool MaxNearestNeighborCrossDeviceScratch(USuperFAISSScratchBank* SourceBank,
		const USuperFAISSVectorBank* TargetBank, float& OutValue);

	// Set-to-set centroid distance between a scratch snapshot's live rows and a baked
	// BankB (the scratch closure of SetToSetDistanceCrossDevice).
	UFUNCTION(BlueprintCallable, Category = "Similarity|Analytics",
		meta = (DisplayName = "Set-To-Set Distance (Cross-Device, Scratch A)"))
	bool SetToSetDistanceCrossDeviceScratch(USuperFAISSScratchBank* BankA,
		const USuperFAISSVectorBank* BankB, const TArray<int32>& RowIndicesB,
		const TArray<int32>& WeightsB, ESuperFAISSBankMetric Metric, float& OutDistance);

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
