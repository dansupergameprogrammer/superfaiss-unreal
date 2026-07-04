#include "SuperFAISSBankLint.h"

#include "SuperFAISSPrototypeAsset.h"
#include "SuperFAISSSubsystem.h"
#include "SuperFAISSVectorBank.h"

#include "superfaiss/superfaiss.h"

namespace
{
	// Dequantized element r,j — the value the kernels score.
	double LintRowElem(const superfaiss::BankView& View, int32 R, int32 J)
	{
		if (View.quant == superfaiss::Quantization::Int8)
		{
			const int8* Row = static_cast<const int8*>(View.rows) +
				static_cast<int64>(R) * View.paddedDims;
			return static_cast<double>(Row[J]) * View.scales[R];
		}
		const float* Row = static_cast<const float*>(View.rows) +
			static_cast<int64>(R) * View.paddedDims;
		return static_cast<double>(Row[J]);
	}
}

bool FSuperFAISSBankLint::FindNearDuplicates(
	const USuperFAISSVectorBank* Bank,
	float Threshold,
	int32 SampleLimit,
	FSuperFAISSLintReport& InOut)
{
	if (Bank == nullptr || !Bank->IsValid() || SampleLimit <= 0)
	{
		return false;
	}
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (Subsystem == nullptr)
	{
		return false;
	}

	const int32 Count = Bank->Count;
	const bool bL2 = Bank->Metric == ESuperFAISSBankMetric::L2;

	// Deterministic stride sample above the limit (N1: never silently exhaustive).
	InOut.bSampled = Count > SampleLimit;
	const int32 Stride = InOut.bSampled ? FMath::DivideAndRoundUp(Count, SampleLimit) : 1;

	TArray<int32> Examined;
	TArray<float> Queries;
	TArray<float> RowQuery;
	for (int32 R = 0; R < Count; R += Stride)
	{
		if (!Subsystem->MakeCentroidQuery(Bank, {R}, RowQuery))
		{
			return false;
		}
		Examined.Add(R);
		Queries.Append(RowQuery);
	}
	InOut.RowsExamined = Examined.Num();

	FSuperFAISSQueryArgs Args;
	Args.K = 2;
	TArray<FSuperFAISSHit> Hits;
	TArray<int32> Counts;
	if (!Subsystem->QueryBatch(Bank, Queries, Examined.Num(), Args, Hits, Counts))
	{
		return false;
	}

	for (int32 E = 0; E < Examined.Num(); ++E)
	{
		if (Counts[E] < 2)
		{
			continue;
		}
		// Rank 1 is the row itself (self-similarity); the runner-up is the candidate.
		// If exact ties displaced self from rank 1, both hits are candidates.
		for (int32 i = 0; i < 2; ++i)
		{
			const FSuperFAISSHit& H = Hits[E * 2 + i];
			if (H.Index == Examined[E])
			{
				continue;
			}
			const bool bTrips = bL2 ? H.Score <= Threshold : H.Score >= Threshold;
			if (bTrips)
			{
				FSuperFAISSNearDuplicate Dup;
				Dup.RowA = FMath::Min(Examined[E], H.Index);
				Dup.RowB = FMath::Max(Examined[E], H.Index);
				Dup.Score = H.Score;
				const bool bAlready = InOut.NearDuplicates.ContainsByPredicate(
					[&Dup](const FSuperFAISSNearDuplicate& X)
					{
						return X.RowA == Dup.RowA && X.RowB == Dup.RowB;
					});
				if (!bAlready)
				{
					InOut.NearDuplicates.Add(Dup);
				}
			}
		}
	}
	return true;
}

bool FSuperFAISSBankLint::FindLowVarianceDims(
	const USuperFAISSVectorBank* Bank,
	float VarianceEpsilon,
	FSuperFAISSLintReport& InOut)
{
	if (Bank == nullptr || !Bank->IsValid() || Bank->Count <= 0)
	{
		return false;
	}
	const superfaiss::BankView View = Bank->GetBankView();

	for (int32 J = 0; J < View.dims; ++J)
	{
		double Mean = 0.0;
		for (int32 R = 0; R < View.count; ++R)
		{
			Mean += LintRowElem(View, R, J);
		}
		Mean /= View.count;
		double Var = 0.0;
		for (int32 R = 0; R < View.count; ++R)
		{
			const double D = LintRowElem(View, R, J) - Mean;
			Var += D * D;
		}
		Var /= View.count;
		if (Var <= VarianceEpsilon)
		{
			InOut.LowVarianceDims.Add(J);
		}
	}
	return true;
}

bool FSuperFAISSBankLint::FindPrototypeOverlaps(
	TConstArrayView<const USuperFAISSPrototypeAsset*> Prototypes,
	float Threshold,
	FSuperFAISSLintReport& InOut)
{
	for (int32 A = 0; A < Prototypes.Num(); ++A)
	{
		for (int32 B = A + 1; B < Prototypes.Num(); ++B)
		{
			const USuperFAISSPrototypeAsset* PA = Prototypes[A];
			const USuperFAISSPrototypeAsset* PB = Prototypes[B];
			if (PA == nullptr || PB == nullptr || PA->Query.Num() == 0 ||
				PA->Query.Num() != PB->Query.Num())
			{
				continue;
			}
			double Dot = 0.0, NA = 0.0, NB = 0.0;
			for (int32 J = 0; J < PA->Query.Num(); ++J)
			{
				Dot += static_cast<double>(PA->Query[J]) * PB->Query[J];
				NA += static_cast<double>(PA->Query[J]) * PA->Query[J];
				NB += static_cast<double>(PB->Query[J]) * PB->Query[J];
			}
			if (NA == 0.0 || NB == 0.0)
			{
				continue;
			}
			const double Cos = Dot / (FMath::Sqrt(NA) * FMath::Sqrt(NB));
			if (Cos >= Threshold)
			{
				FSuperFAISSPrototypeOverlap Overlap;
				Overlap.PrototypeA = A;
				Overlap.PrototypeB = B;
				Overlap.CosineSimilarity = static_cast<float>(Cos);
				InOut.PrototypeOverlaps.Add(Overlap);
			}
		}
	}
	return true;
}
