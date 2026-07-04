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
	 * axis-projection path on L2 banks; identity elsewhere.
	 */
	UFUNCTION(meta = (AICallable), Category = "SuperFAISS")
	static FString QueryBank(const FString& BankPath, const FString& RowId,
		int32 RowIndex, const TArray<float>& Vector, int32 K = 10,
		bool bScoreAsDot = false);

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
	 * above SampleLimit — the report says so), low-variance dimensions. Cost scales
	 * with bank size; run on demand, not routinely.
	 */
	UFUNCTION(meta = (AICallable), Category = "SuperFAISS")
	static FString LintBank(const FString& BankPath, float DuplicateThreshold = 0.999f,
		int32 SampleLimit = 4096, float VarianceEpsilon = 0.00000001f);
};
