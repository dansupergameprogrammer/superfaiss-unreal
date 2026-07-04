// Query-composition surface tests (plan 18.1 / V1.1 slot A): centroid and direction
// helpers, the per-query dot-scoring override, and margin output, at the subsystem API.
// The metric x quantization matrix depth lives core-side (T14-T17); these prove the
// UE surface end to end.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/IConsoleManager.h"
#include "SuperFAISSQueryProvider.h"
#include "SuperFAISSSubsystem.h"
#include "SuperFAISSVectorBank.h"

namespace
{
	TArray<float> CompositionRows(int32 Count, int32 Dims, uint64 Seed)
	{
		TArray<float> Rows;
		Rows.SetNumUninitialized(Count * Dims);
		uint64 State = Seed;
		for (float& V : Rows)
		{
			State ^= State >> 12;
			State ^= State << 25;
			State ^= State >> 27;
			V = static_cast<float>(static_cast<int64>((State * 0x2545F4914F6CDD1Dull) >> 24)) /
				static_cast<float>(1ll << 39);
		}
		return Rows;
	}

	USuperFAISSVectorBank* CompositionBank(FAutomationTestBase& Test,
		const TArray<float>& Rows, int32 Count, int32 Dims, ESuperFAISSBankMetric Metric)
	{
		USuperFAISSVectorBank* Bank = NewObject<USuperFAISSVectorBank>();
		FString Error;
		const bool bOk = Bank->InitFromSource(Rows, Count, Dims, Metric,
			ESuperFAISSBankQuantization::Float32, {}, TEXT("composition-test"), Error);
		Test.TestTrue(FString::Printf(TEXT("bank built: %s"), *Error), bOk);
		return bOk ? Bank : nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSQueryCompositionTest,
	"SuperFAISS.A.QueryComposition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSQueryCompositionTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	constexpr int32 Count = 500;
	constexpr int32 Dims = 24;
	const TArray<float> Rows = CompositionRows(Count, Dims, 0xC0135ull);

	// --- Centroid: mean of chosen rows on a Dot bank equals the hand-computed mean.
	{
		USuperFAISSVectorBank* Bank =
			CompositionBank(*this, Rows, Count, Dims, ESuperFAISSBankMetric::Dot);
		if (!Bank)
		{
			return true;
		}
		const TArray<int32> Chosen = {5, 41, 300};
		TArray<float> Centroid;
		TestTrue(TEXT("centroid built"),
			Subsystem->MakeCentroidQuery(Bank, Chosen, Centroid));
		TestEqual(TEXT("centroid dims"), Centroid.Num(), Dims);
		for (int32 J = 0; J < Dims && J < Centroid.Num(); ++J)
		{
			double Ref = 0.0;
			for (int32 RowIndex : Chosen)
			{
				Ref += Rows[RowIndex * Dims + J];
			}
			Ref /= Chosen.Num();
			TestTrue(FString::Printf(TEXT("centroid dim %d: %g vs %g"), J, Centroid[J], Ref),
				FMath::Abs(Centroid[J] - Ref) <= 1e-5 * (1.0 + FMath::Abs(Ref)));
		}

		// The centroid queries its own bank successfully.
		FSuperFAISSQueryArgs Args;
		Args.K = 5;
		TArray<FSuperFAISSHit> Hits;
		TestTrue(TEXT("centroid queries"), Subsystem->QuerySync(Bank, Centroid, Args, Hits));
		TestTrue(TEXT("centroid hit count"), Hits.Num() > 0);

		// Rejections: empty selection, out-of-range index.
		TArray<float> Rejected;
		TestFalse(TEXT("empty selection rejected"),
			Subsystem->MakeCentroidQuery(Bank, {}, Rejected));
		TestFalse(TEXT("bad index rejected"),
			Subsystem->MakeCentroidQuery(Bank, {Count}, Rejected));
	}

	// --- Centroid zero-norm rejection: antipodal members on a Cosine bank cancel.
	{
		TArray<float> Anti;
		Anti.SetNumZeroed(2 * Dims);
		Anti[0] = 1.0f;
		Anti[Dims] = -1.0f;
		USuperFAISSVectorBank* Bank =
			CompositionBank(*this, Anti, 2, Dims, ESuperFAISSBankMetric::Cosine);
		if (Bank)
		{
			TArray<float> Rejected;
			TestFalse(TEXT("zero-norm centroid rejected"),
				Subsystem->MakeCentroidQuery(Bank, {0, 1}, Rejected));
		}
	}

	// --- Direction: normalize(A - B), hand-checked; A == B rejected.
	{
		TArray<float> A;
		TArray<float> B;
		A.SetNumZeroed(Dims);
		B.SetNumZeroed(Dims);
		A[1] = 2.0f;
		A[2] = 2.0f;
		TArray<float> Direction;
		TestTrue(TEXT("direction built"), Subsystem->MakeDirectionQuery(A, B, Direction));
		const float Inv = 1.0f / FMath::Sqrt(8.0f);
		TestTrue(TEXT("direction dim1"), FMath::Abs(Direction[1] - 2.0f * Inv) <= 1e-6f);
		TestTrue(TEXT("direction dim2"), FMath::Abs(Direction[2] - 2.0f * Inv) <= 1e-6f);
		TestFalse(TEXT("A==B rejected"), Subsystem->MakeDirectionQuery(A, A, Direction));
	}

	// --- Dot-scoring override: identity on a Cosine bank; on an L2 bank, bit-identical
	// to a Dot bank baked from the same rows (same payload, different metric tag).
	{
		USuperFAISSVectorBank* CosineBank =
			CompositionBank(*this, Rows, Count, Dims, ESuperFAISSBankMetric::Cosine);
		USuperFAISSVectorBank* L2Bank =
			CompositionBank(*this, Rows, Count, Dims, ESuperFAISSBankMetric::L2);
		USuperFAISSVectorBank* DotBank =
			CompositionBank(*this, Rows, Count, Dims, ESuperFAISSBankMetric::Dot);
		if (!CosineBank || !L2Bank || !DotBank)
		{
			return true;
		}
		TArray<float> Query = CompositionRows(1, Dims, 0xD07ull);

		FSuperFAISSQueryArgs Plain;
		Plain.K = 10;
		FSuperFAISSQueryArgs AsDot = Plain;
		AsDot.bScoreAsDot = true;

		TArray<FSuperFAISSHit> CosPlain, CosOverride;
		TestTrue(TEXT("cosine plain"), Subsystem->QuerySync(CosineBank, Query, Plain, CosPlain));
		TestTrue(TEXT("cosine override"),
			Subsystem->QuerySync(CosineBank, Query, AsDot, CosOverride));
		TestEqual(TEXT("cosine identity count"), CosOverride.Num(), CosPlain.Num());
		for (int32 i = 0; i < CosPlain.Num() && i < CosOverride.Num(); ++i)
		{
			TestTrue(TEXT("cosine identity hit"),
				CosOverride[i].Index == CosPlain[i].Index &&
				CosOverride[i].Score == CosPlain[i].Score);
		}

		TArray<FSuperFAISSHit> L2AsDot, DotPlain;
		TestTrue(TEXT("l2 override"), Subsystem->QuerySync(L2Bank, Query, AsDot, L2AsDot));
		TestTrue(TEXT("dot plain"), Subsystem->QuerySync(DotBank, Query, Plain, DotPlain));
		TestEqual(TEXT("projection count"), L2AsDot.Num(), DotPlain.Num());
		for (int32 i = 0; i < L2AsDot.Num() && i < DotPlain.Num(); ++i)
		{
			TestTrue(TEXT("projection hit bit-identical"),
				L2AsDot[i].Index == DotPlain[i].Index &&
				L2AsDot[i].Score == DotPlain[i].Score);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSMarginOutputTest,
	"SuperFAISS.A.MarginOutput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSMarginOutputTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	constexpr int32 Count = 800;
	constexpr int32 Dims = 16;
	const TArray<float> Rows = CompositionRows(Count, Dims, 0xA4611ull);

	for (ESuperFAISSBankMetric Metric :
		{ESuperFAISSBankMetric::Cosine, ESuperFAISSBankMetric::L2})
	{
		USuperFAISSVectorBank* Bank = CompositionBank(*this, Rows, Count, Dims, Metric);
		if (!Bank)
		{
			continue;
		}
		const TArray<float> Query = CompositionRows(1, Dims, 0x9147ull);

		FSuperFAISSQueryArgs Args;
		Args.K = 8;
		TArray<FSuperFAISSHit> Hits;
		TestTrue(TEXT("query"), Subsystem->QuerySync(Bank, Query, Args, Hits));
		const bool bL2 = Metric == ESuperFAISSBankMetric::L2;
		for (int32 i = 0; i < Hits.Num(); ++i)
		{
			if (i + 1 < Hits.Num())
			{
				const float Expect = bL2 ? Hits[i + 1].Score - Hits[i].Score
				                         : Hits[i].Score - Hits[i + 1].Score;
				TestTrue(TEXT("margin non-negative"), Hits[i].Margin >= 0.0f);
				TestEqual(TEXT("margin exact"), Hits[i].Margin, Expect);
			}
			else
			{
				TestEqual(TEXT("last margin zero"), Hits[i].Margin, 0.0f);
			}
		}

		// Batch margins equal single margins (batch is bit-identical to singles, so
		// per-hit gaps must be too).
		TArray<float> Two;
		Two.Append(Query);
		Two.Append(CompositionRows(1, Dims, 0x9148ull));
		TArray<FSuperFAISSHit> BatchHits;
		TArray<int32> BatchCounts;
		TestTrue(TEXT("batch"),
			Subsystem->QueryBatch(Bank, Two, 2, Args, BatchHits, BatchCounts));
		if (BatchCounts.Num() == 2 && BatchCounts[0] == Hits.Num())
		{
			for (int32 i = 0; i < Hits.Num(); ++i)
			{
				TestEqual(TEXT("batch margin equals single"),
					BatchHits[i].Margin, Hits[i].Margin);
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSIntersectCompositionTest,
	"SuperFAISS.A.IntersectComposition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSIntersectCompositionTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	constexpr int32 Count = 300;
	constexpr int32 Dims = 20;
	const TArray<float> Rows = CompositionRows(Count, Dims, 0x15EC7ull);

	for (ESuperFAISSBankMetric Metric :
		{ESuperFAISSBankMetric::Cosine, ESuperFAISSBankMetric::L2})
	{
		USuperFAISSVectorBank* Bank = CompositionBank(*this, Rows, Count, Dims, Metric);
		if (!Bank)
		{
			continue;
		}
		const bool bL2 = Metric == ESuperFAISSBankMetric::L2;
		const TArray<float> QueryA = CompositionRows(1, Dims, 0xAAA1ull);
		const TArray<float> QueryB = CompositionRows(1, Dims, 0xBBB2ull);

		// Reference: full rankings of each member query (K = Count returns every
		// row), fused worst-of per row, sorted by (score, index) total order.
		FSuperFAISSQueryArgs All;
		All.K = Count;
		TArray<FSuperFAISSHit> FullA, FullB;
		TestTrue(TEXT("full A"), Subsystem->QuerySync(Bank, QueryA, All, FullA));
		TestTrue(TEXT("full B"), Subsystem->QuerySync(Bank, QueryB, All, FullB));
		TArray<float> ScoreA, ScoreB;
		ScoreA.SetNumZeroed(Count);
		ScoreB.SetNumZeroed(Count);
		for (const FSuperFAISSHit& H : FullA) { ScoreA[H.Index] = H.Score; }
		for (const FSuperFAISSHit& H : FullB) { ScoreB[H.Index] = H.Score; }

		struct FRef { int32 Index; float Fused; };
		TArray<FRef> Ref;
		Ref.Reserve(Count);
		for (int32 R = 0; R < Count; ++R)
		{
			const float Fused = bL2 ? FMath::Max(ScoreA[R], ScoreB[R])
			                        : FMath::Min(ScoreA[R], ScoreB[R]);
			Ref.Add({R, Fused});
		}
		Ref.Sort([bL2](const FRef& X, const FRef& Y)
		{
			if (X.Fused != Y.Fused)
			{
				return bL2 ? X.Fused < Y.Fused : X.Fused > Y.Fused;
			}
			return X.Index < Y.Index;
		});

		constexpr int32 K = 12;
		TArray<FSuperFAISSHit> Got;
		TestTrue(TEXT("intersect"),
			Subsystem->QuerySimilarIntersect(Bank, QueryA, QueryB, K, Got));
		TestEqual(TEXT("intersect count"), Got.Num(), K);
		for (int32 i = 0; i < Got.Num(); ++i)
		{
			TestEqual(TEXT("fused index"), Got[i].Index, Ref[i].Index);
			TestEqual(TEXT("fused score"), Got[i].Score, Ref[i].Fused);
		}

		// Degeneracy at the API surface: intersect(Q, Q) equals the plain query.
		FSuperFAISSQueryArgs Plain;
		Plain.K = K;
		TArray<FSuperFAISSHit> Single, Degen;
		TestTrue(TEXT("plain"), Subsystem->QuerySync(Bank, QueryA, Plain, Single));
		TestTrue(TEXT("degen"),
			Subsystem->QuerySimilarIntersect(Bank, QueryA, QueryA, K, Degen));
		TestEqual(TEXT("degen count"), Degen.Num(), Single.Num());
		for (int32 i = 0; i < FMath::Min(Degen.Num(), Single.Num()); ++i)
		{
			TestTrue(TEXT("degen hit"),
				Degen[i].Index == Single[i].Index && Degen[i].Score == Single[i].Score);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSIntersectSerialParallelTest,
	"SuperFAISS.B.IntersectSerialParallelEquality",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSIntersectSerialParallelTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}
	// Big enough for a real chunk fan-out (mirrors B.SerialParallelEquality sizing).
	constexpr int32 Count = 12000;
	constexpr int32 Dims = 96;
	const TArray<float> Rows = CompositionRows(Count, Dims, 0x5EBAull);
	USuperFAISSVectorBank* Bank =
		CompositionBank(*this, Rows, Count, Dims, ESuperFAISSBankMetric::Cosine);
	if (!Bank)
	{
		return true;
	}

	IConsoleVariable* ModeVar =
		IConsoleManager::Get().FindConsoleVariable(TEXT("superfaiss.ParallelScan"));
	if (!TestNotNull(TEXT("cvar exists"), ModeVar))
	{
		return true;
	}
	const int32 SavedMode = ModeVar->GetInt();

	const TArray<float> QueryA = CompositionRows(1, Dims, 0xE1ull);
	const TArray<float> QueryB = CompositionRows(1, Dims, 0xE2ull);

	TArray<FSuperFAISSHit> Serial, Parallel;
	ModeVar->Set(0);
	TestTrue(TEXT("serial"),
		Subsystem->QuerySimilarIntersect(Bank, QueryA, QueryB, 20, Serial));
	ModeVar->Set(2);
	TestTrue(TEXT("parallel"),
		Subsystem->QuerySimilarIntersect(Bank, QueryA, QueryB, 20, Parallel));
	ModeVar->Set(SavedMode);

	TestEqual(TEXT("counts"), Parallel.Num(), Serial.Num());
	for (int32 i = 0; i < FMath::Min(Serial.Num(), Parallel.Num()); ++i)
	{
		TestTrue(TEXT("bit-identical"),
			Serial[i].Index == Parallel[i].Index && Serial[i].Score == Parallel[i].Score &&
			Serial[i].Margin == Parallel[i].Margin);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSQueryProviderSeamTest,
	"SuperFAISS.A.QueryProviderSeam",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSQueryProviderSeamTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	constexpr int32 Count = 64;
	constexpr int32 Dims = 16;
	const TArray<float> Rows = CompositionRows(Count, Dims, 0x5EA11ull);
	TArray<FName> Ids;
	for (int32 R = 0; R < Count; ++R)
	{
		Ids.Add(FName(*FString::Printf(TEXT("row%d"), R)));
	}
	USuperFAISSVectorBank* Bank = NewObject<USuperFAISSVectorBank>();
	FString Error;
	if (!TestTrue(TEXT("bank built"), Bank->InitFromSource(Rows, Count, Dims,
		ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Float32, Ids,
		TEXT("seam-test"), Error)))
	{
		return true;
	}

	// Reverse lookup round-trips.
	TestEqual(TEXT("index for id"), Bank->GetIndexForId(TEXT("row17")), 17);
	TestEqual(TEXT("missing id"), Bank->GetIndexForId(TEXT("nope")), (int32)INDEX_NONE);

	// The reference provider produces a query from a row id; querying with it puts
	// that row at rank 1 with self-similarity ~1 on the Cosine bank.
	USuperFAISSBankRowQueryProvider* Provider =
		NewObject<USuperFAISSBankRowQueryProvider>();
	Provider->RowId = TEXT("row17");
	TArray<float> Query;
	TestTrue(TEXT("provider produced"),
		ISuperFAISSQueryProvider::Execute_GetQueryVector(Provider, Bank, Query));
	TestEqual(TEXT("provider dims"), Query.Num(), Dims);

	FSuperFAISSQueryArgs Args;
	Args.K = 3;
	TArray<FSuperFAISSHit> Hits;
	TestTrue(TEXT("provider query"), Subsystem->QuerySync(Bank, Query, Args, Hits));
	if (TestTrue(TEXT("has hits"), Hits.Num() > 0))
	{
		TestEqual(TEXT("self at rank 1"), Hits[0].Index, 17);
		TestTrue(TEXT("self-similarity ~1"), FMath::Abs(Hits[0].Score - 1.0f) <= 1e-3f);
	}

	// Provider fallbacks and failures: index path, out-of-range, no source set.
	USuperFAISSBankRowQueryProvider* ByIndex = NewObject<USuperFAISSBankRowQueryProvider>();
	ByIndex->RowIndex = 5;
	TestTrue(TEXT("index path"),
		ISuperFAISSQueryProvider::Execute_GetQueryVector(ByIndex, Bank, Query));
	USuperFAISSBankRowQueryProvider* Bad = NewObject<USuperFAISSBankRowQueryProvider>();
	Bad->RowIndex = Count;
	TestFalse(TEXT("out of range"),
		ISuperFAISSQueryProvider::Execute_GetQueryVector(Bad, Bank, Query));
	USuperFAISSBankRowQueryProvider* Unset = NewObject<USuperFAISSBankRowQueryProvider>();
	TestFalse(TEXT("unset provider"),
		ISuperFAISSQueryProvider::Execute_GetQueryVector(Unset, Bank, Query));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

