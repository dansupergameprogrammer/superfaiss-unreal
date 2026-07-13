#pragma once

#include "CoreMinimal.h"

#include "ToolsetRegistry/ToolsetDefinition.h"

#include "SuperFAISSToolset.generated.h"

/**
 * SuperFAISS vector-bank tools: enumerate, describe, query, import, validate, and
 * lint the project's embedding banks. Queries are exact k-nearest-neighbor search
 * (Dot, Cosine, or L2), deterministic per device, typically sub-millisecond on
 * game-scale banks. Read/import-only: no tool deletes or mutates an existing bank.
 * All tools run synchronously in the editor; ImportBank and LintBank note their
 * costs in their own descriptions.
 */
UCLASS()
class SUPERFAISSUNREALMCP_API USuperFAISSToolset : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	/** Connectivity probe: echoes the message back and names the executing thread. */
	UFUNCTION(meta = (AICallable), Category = "SuperFAISS")
	static FString Echo(const FString& Message);

	/**
	 * Lists every SuperFAISS vector bank in the project: asset path, row count,
	 * dimensions, metric, quantization, and payload size. Loads bank assets to read
	 * their headers.
	 */
	UFUNCTION(meta = (AICallable), Category = "SuperFAISS")
	static FString ListBanks();

	/**
	 * Full metadata for one bank (by asset path, e.g. /SuperFAISSUnreal/Demo/DemoBank):
	 * dimensions, count, metric, quantization, payload bytes, whether rows carry ids,
	 * the measured quantization recall@10, and the source-sidecar hash.
	 */
	UFUNCTION(meta = (AICallable), Category = "SuperFAISS")
	static FString DescribeBank(const FString& BankPath);

	/**
	 * Exact top-K query against a bank. Provide exactly one query source: RowId (for
	 * banks whose rows carry ids — the id IS the row; there is no text encoder),
	 * RowIndex (>= 0), or Vector (raw floats, must match the bank's dimensions).
	 * Returns ranked hits with index, id, score, and margin (the gap to the next
	 * hit). ScoreAsDot scores with the dot kernel regardless of bank metric — the
	 * axis-projection path on L2 banks; identity elsewhere. On channel banks
	 * (schemaVersion 2), ChannelNames + ChannelWeights (parallel arrays) rank by a
	 * weighted combination of named channels; on Cosine channel banks each
	 * channel's score is a true per-channel cosine. Empty = whole-row query.
	 * BiasIndices + BiasValues (parallel arrays, v2.1) add a per-row score bias
	 * IN-SCAN, so the composed ranking is exact: indices are bank rows (unique),
	 * values finite, added in the scored metric's own direction (a reward is
	 * negative on L2). The sparse pairs form is the only bias form MCP exposes -
	 * a count-length float array through JSON is not an agent surface.
	 * bCrossDeviceExact (v2.2) runs the cross-device exactness mode: bit-identical
	 * scores and hit order on any machine at any SIMD width; int8 banks only, and
	 * the bank asset carries the mode's measured recall (crossDeviceRecallAt10).
	 */
	UFUNCTION(meta = (AICallable), Category = "SuperFAISS")
	static FString QueryBank(const FString& BankPath, const FString& RowId,
		int32 RowIndex, const TArray<float>& Vector,
		const TArray<FString>& ChannelNames, const TArray<float>& ChannelWeights,
		const TArray<int32>& BiasIndices, const TArray<float>& BiasValues,
		int32 K = 10, bool bScoreAsDot = false, bool bCrossDeviceExact = false);

	/**
	 * Query by prototype ("the category's center"): the centroid of the listed rows
	 * (ids and/or indices), or a saved prototype asset (PrototypeAssetPath). A
	 * centroid whose members cancel to zero norm on a Cosine bank is an error, not a
	 * silent renormalization. Returns ranked hits like QueryBank.
	 */
	UFUNCTION(meta = (AICallable), Category = "SuperFAISS")
	static FString QueryPrototype(const FString& BankPath, const TArray<FString>& RowIds,
		const TArray<int32>& RowIndices, const FString& PrototypeAssetPath,
		int32 K = 10);

	/**
	 * Rank the bank along the direction from VectorB toward VectorA — "most A-like
	 * relative to B". The direction is normalized; A == B is an error. On Dot banks
	 * the score includes row magnitude; Cosine banks give pure direction ranking and
	 * are the recommended substrate. On L2 banks this forces dot scoring.
	 */
	UFUNCTION(meta = (AICallable), Category = "SuperFAISS")
	static FString ProjectAxis(const FString& BankPath, const TArray<float>& VectorA,
		const TArray<float>& VectorB, int32 K = 10);

	/**
	 * Imports a sidecar pair (<name>.wvbank.json + .bin) as a bank asset with full
	 * validation and a seeded recall report; can take seconds on large banks. The
	 * destination must be under /Game. An existing asset at the destination is
	 * refused unless bAllowOverwrite is true. Quantization is Int8 or Float32.
	 */
	UFUNCTION(meta = (AICallable), Category = "SuperFAISS")
	static FString ImportBank(const FString& SidecarJsonPath,
		const FString& DestinationPackagePath, const FString& Quantization,
		bool bAllowOverwrite = false);

	/** Validates every bank in the project; per-bank pass/fail with diagnostics. */
	UFUNCTION(meta = (AICallable), Category = "SuperFAISS")
	static FString ValidateBanks();

	/**
	 * Health analyses for one bank: near-duplicate rows (examined rows are sampled
	 * above SampleLimit — the report says so), low-variance dimensions; on channel
	 * banks also per-channel near-duplicates, degenerate channels, and weak
	 * channels (rows whose channel carries almost no energy). Cost scales with
	 * bank size; run on demand, not routinely.
	 */
	UFUNCTION(meta = (AICallable), Category = "SuperFAISS")
	static FString LintBank(const FString& BankPath, float DuplicateThreshold = 0.999f,
		int32 SampleLimit = 4096, float VarianceEpsilon = 0.00000001f);

	/**
	 * Probe-direction projection audit (V2.5, plan section 22.3). Projects every row of
	 * the bank onto the unit direction from VectorB toward VectorA (normalize(A - B),
	 * the same direction primitive ProjectAxis uses) and reports the whole-set
	 * projections — ProjectAxis's top-K ranking widened to a full-bank audit. When
	 * GroupA (row indices) is non-empty, also reports the Cohen's-d separation of that
	 * group's projected mean against the remaining rows: the constitution's bake-time
	 * anchor-audit instrument. Per-device float, offline. VectorA/B must match the
	 * bank's dimensions; A == B, an empty bank, or a GroupA covering every row is an
	 * error. Read-only.
	 */
	UFUNCTION(meta = (AICallable), Category = "SuperFAISS")
	static FString ProjectionReport(const FString& BankPath, const TArray<float>& VectorA,
		const TArray<float>& VectorB, const TArray<int32>& GroupA);

	/**
	 * Set-to-set divergence between two int8 cross-device banks (V2.5, plan section
	 * 22.4). Mode selects the read: "centroid" (the cross-device centroid distance of
	 * the selected rows, RowIndicesA/RowIndicesB — empty selects all rows), "meanNN" or
	 * "maxNN" (directed nearest-neighbour divergence from A's rows to B's, whole-bank),
	 * or "all" (default, all three). Metric ("Dot"/"Cosine"/"L2", empty = bank A's
	 * metric) sets the centroid distance sense; the NN divergences score in bank B's
	 * own metric. DRIFT over checkpoints is this read applied to two checkpoint bank
	 * paths. Both banks int8 cross-device with equal dimensions. Read-only.
	 */
	UFUNCTION(meta = (AICallable), Category = "SuperFAISS")
	static FString SetToSetDistance(const FString& BankPathA, const FString& BankPathB,
		const TArray<int32>& RowIndicesA, const TArray<int32>& RowIndicesB,
		const FString& Metric, const FString& Mode);

	/**
	 * Within-bank dispersion (V2.5, plan section 22.4): the mean and max distance of
	 * each selected row (RowIndices — empty selects all rows) to the selection's own
	 * cross-device centroid, in the bank's metric. The int8 cross-device spread
	 * statistic. Int8 banks only. Read-only.
	 */
	UFUNCTION(meta = (AICallable), Category = "SuperFAISS")
	static FString BankSpread(const FString& BankPath, const TArray<int32>& RowIndices);

	/**
	 * Lists live scratch banks (runtime, mutable, in-memory — NPC memory and other
	 * session-accumulated embeddings; they are not assets). Read-only: scratch
	 * banks are mutated by game code, never through MCP.
	 */
	UFUNCTION(meta = (AICallable), Category = "SuperFAISS")
	static FString ListScratchBanks();

	/**
	 * Metadata for one live scratch bank, by the object path ListScratchBanks
	 * reported. States the float-retention flag (a dev/audit posture set at Init)
	 * and, when game code has measured one, the recall-audit report with its
	 * generation stamp and stale mark — a report taken before a later append or
	 * remove reads as stale, never silently current.
	 */
	UFUNCTION(meta = (AICallable), Category = "SuperFAISS")
	static FString DescribeScratchBank(const FString& ScratchBankPath);

	/**
	 * Exact top-K query against a live scratch bank (by object path). Vector query
	 * only — scratch rows carry no ids. Removed rows are excluded automatically.
	 */
	UFUNCTION(meta = (AICallable), Category = "SuperFAISS")
	static FString QueryScratchBank(const FString& ScratchBankPath,
		const TArray<float>& Vector, int32 K = 10);

	/**
	 * Set-to-set divergence from a live scratch bank's snapshot (source A, by object
	 * path) to a baked int8 target bank B (plan section 23.3, slot 1). The scratch
	 * closure of SetToSetDistance: A is the mutable snapshot (all live rows, tombstones
	 * excluded), B the baked reference (RowIndicesB — empty selects all). Mode selects
	 * the read: "centroid" (SetToSetDistanceCrossDeviceScratch), "meanNN"
	 * (MeanNearestNeighborCrossDeviceScratch), "maxNN"
	 * (MaxNearestNeighborCrossDeviceScratch), or "all" (default, all three). Metric
	 * ("Dot"/"Cosine"/"L2", empty = bank B's metric) sets the centroid distance sense.
	 * Read-only: the scratch bank is snapshotted, never mutated or baked.
	 */
	UFUNCTION(meta = (AICallable), Category = "SuperFAISS")
	static FString SetToSetDistanceScratch(const FString& ScratchBankPathA,
		const FString& BankPathB, const TArray<int32>& RowIndicesB,
		const FString& Metric, const FString& Mode);

	/**
	 * Within-scratch dispersion (plan section 23.3, slot 1): the mean and max distance
	 * of a live scratch bank's snapshot rows to the snapshot's own cross-device centroid,
	 * in the bank's metric (the scratch closure of BankSpread via
	 * BankSpreadCrossDeviceScratch). Read-only: the scratch bank is snapshotted, never
	 * mutated or baked.
	 */
	UFUNCTION(meta = (AICallable), Category = "SuperFAISS")
	static FString BankSpreadScratch(const FString& ScratchBankPath);

	/**
	 * Channel-scoped set-to-set divergence between two int8 channel banks (V3.0, plan
	 * section 23.5, slot 5): as SetToSetDistance but scored over one named channel's
	 * sub-range instead of the whole row. Channel is a channel-table index (the bank's
	 * schemaVersion-2 named channels). Mode selects the read ("centroid"/"meanNN"/
	 * "maxNN"/"all"); Metric sets the centroid distance sense. Both banks int8
	 * cross-device with a channel table. Read-only.
	 */
	UFUNCTION(meta = (AICallable), Category = "SuperFAISS")
	static FString SetToSetDistanceChannel(const FString& BankPathA, const FString& BankPathB,
		const TArray<int32>& RowIndicesA, const TArray<int32>& RowIndicesB,
		const FString& Metric, const FString& Mode, int32 Channel);

	/**
	 * Channel-scoped within-bank dispersion (V3.0, plan section 23.5, slot 5): the mean
	 * and max distance of each selected row (RowIndices — empty selects all) to the
	 * selection's own cross-device centroid, scored over one named channel's sub-range
	 * (Channel is a channel-table index). Int8 channel banks only. Read-only.
	 */
	UFUNCTION(meta = (AICallable), Category = "SuperFAISS")
	static FString BankSpreadChannel(const FString& BankPath, const TArray<int32>& RowIndices,
		int32 Channel);

	/**
	 * Health lint over a live channel scratch bank (V3.0, plan section 23.4 / 23.9 slot
	 * 5): per-channel degenerate/weak-channel sub-norm-floor warnings — a channel whose
	 * rows carry near-zero energy makes its per-channel scores unreliable, and the
	 * report names it. Read-only: scratch banks are mutated by game code, never through
	 * MCP.
	 */
	UFUNCTION(meta = (AICallable), Category = "SuperFAISS")
	static FString LintScratchBank(const FString& ScratchBankPath);
};
