#pragma once

#include "CoreMinimal.h"

class USuperFAISSVectorBank;
class USuperFAISSPrototypeAsset;

// Bank health analyses (plan 18.2): near-duplicate rows, degenerate/low-variance
// dims, and prototype-category overlap. On demand only (Forge strike-4 N1):
// near-duplicate detection costs one full scan per examined row, so above
// SampleLimit rows it examines a deterministic stride sample and says so in the
// report — it never silently runs exhaustively on a large import.

struct FSuperFAISSNearDuplicate
{
	int32 RowA = INDEX_NONE;
	int32 RowB = INDEX_NONE;
	// The runner-up score that tripped the threshold, in the bank's metric.
	float Score = 0.0f;
};

struct FSuperFAISSPrototypeOverlap
{
	int32 PrototypeA = INDEX_NONE;
	int32 PrototypeB = INDEX_NONE;
	float CosineSimilarity = 0.0f;
};

struct FSuperFAISSLintReport
{
	TArray<FSuperFAISSNearDuplicate> NearDuplicates;
	TArray<int32> LowVarianceDims;
	TArray<FSuperFAISSPrototypeOverlap> PrototypeOverlaps;
	int32 RowsExamined = 0;
	bool bSampled = false;
};

class SUPERFAISSUNREALEDITOR_API FSuperFAISSBankLint
{
public:
	// Near-duplicates: each examined row queries the bank (K=2, one batch); the
	// runner-up crossing the threshold flags the pair. Threshold semantics follow
	// the bank metric: Dot/Cosine flag runner-up score >= Threshold; L2 flags
	// runner-up distance <= Threshold. Pairs are reported once (RowA < RowB).
	// Above SampleLimit rows, a deterministic stride sample is examined instead
	// (bSampled reports it).
	static bool FindNearDuplicates(
		const USuperFAISSVectorBank* Bank,
		float Threshold,
		int32 SampleLimit,
		FSuperFAISSLintReport& InOut);

	// Dims whose variance across all rows falls at-or-under VarianceEpsilon —
	// dead payload that inflates memory and dilutes distances.
	static bool FindLowVarianceDims(
		const USuperFAISSVectorBank* Bank,
		float VarianceEpsilon,
		FSuperFAISSLintReport& InOut);

	// Pairwise cosine similarity between prototype vectors; pairs at-or-over
	// Threshold are reported as overlapping categories. Indices refer to the
	// Prototypes array order.
	static bool FindPrototypeOverlaps(
		TConstArrayView<const USuperFAISSPrototypeAsset*> Prototypes,
		float Threshold,
		FSuperFAISSLintReport& InOut);
};
