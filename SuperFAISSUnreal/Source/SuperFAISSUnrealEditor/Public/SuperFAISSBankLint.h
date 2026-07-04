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
	// The channel the pair collided in; NAME_None = the whole row.
	FName Channel;
};

struct FSuperFAISSPrototypeOverlap
{
	int32 PrototypeA = INDEX_NONE;
	int32 PrototypeB = INDEX_NONE;
	float CosineSimilarity = 0.0f;
};

// A channel carrying too little row energy in too many rows (T-044 W2c): its
// per-channel cosines are amplified quantization noise and unreliable.
struct FSuperFAISSWeakChannel
{
	FName Channel;
	int32 RowsBelowFloor = 0;
	float WorstEnergyFraction = 1.0f;
};

struct FSuperFAISSLintReport
{
	TArray<FSuperFAISSNearDuplicate> NearDuplicates;
	TArray<int32> LowVarianceDims;
	TArray<FSuperFAISSPrototypeOverlap> PrototypeOverlaps;
	TArray<FSuperFAISSWeakChannel> WeakChannels;
	TArray<FName> DegenerateChannels;
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

	// Per-channel near-duplicates (plan section 11): the whole-row scan scoped to
	// one named channel - two rows that collide in "appearance" while differing in
	// "identity" are invisible to the whole-row pass and are exactly the pairs a
	// channel query will conflate. Same sampling and threshold semantics as above;
	// found pairs carry the channel name.
	static bool FindNearDuplicatesInChannel(
		const USuperFAISSVectorBank* Bank,
		FName Channel,
		float Threshold,
		int32 SampleLimit,
		FSuperFAISSLintReport& InOut);

	// Degenerate channels (plan section 11): a channel whose EVERY dim has variance
	// at-or-under VarianceEpsilon across the bank is authoring noise - its
	// per-channel scores are constant and carry no signal. Reported by name.
	static bool FindDegenerateChannels(
		const USuperFAISSVectorBank* Bank,
		float VarianceEpsilon,
		FSuperFAISSLintReport& InOut);

	// Sub-norm floor (T-044 W2c): on Cosine channel banks, counts rows whose channel
	// carries less than EnergyFloor of the row's energy (sub-norm squared; rows are
	// whole-normalized so the fraction IS the squared sub-norm). Channels with such
	// rows produce unreliable per-channel cosines and are reported per channel.
	static bool FindWeakChannels(
		const USuperFAISSVectorBank* Bank,
		float EnergyFloor,
		FSuperFAISSLintReport& InOut);

	// Pairwise cosine similarity between prototype vectors; pairs at-or-over
	// Threshold are reported as overlapping categories. Indices refer to the
	// Prototypes array order.
	static bool FindPrototypeOverlaps(
		TConstArrayView<const USuperFAISSPrototypeAsset*> Prototypes,
		float Threshold,
		FSuperFAISSLintReport& InOut);
};
