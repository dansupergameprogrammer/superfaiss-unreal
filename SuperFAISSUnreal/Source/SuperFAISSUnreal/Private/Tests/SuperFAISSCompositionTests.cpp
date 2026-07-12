// Query-composition surface tests (plan 18.1 / V1.1 slot A): centroid and direction
// helpers, the per-query dot-scoring override, and margin output, at the subsystem API.
// The metric x quantization matrix depth lives core-side (T14-T17); these prove the
// UE surface end to end.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/IConsoleManager.h"
#include "SuperFAISSQueryProvider.h"
#include "SuperFAISSScratchBank.h"
#include "SuperFAISSSubsystem.h"
#include "SuperFAISSVectorBank.h"

#include "superfaiss/superfaiss.h"

#include <limits>

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

// T-V2.4 subsystem surface — integer-domain pooling (V2 plan section 21):
// MakeCentroidQueryCrossDevice wraps the core operator (the payload byte-equals a
// direct core MakeCentroidCrossDevice over the same view — one operator, one math),
// QueryPooledCrossDevice executes that exact payload through core QueryXd under the
// subsystem's dispatch (hit lists bit-equal a direct core QueryXd), all-equal
// weights bit-equal unweighted, and f32 banks are the defined rejection.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSPooledCrossDeviceTest,
	"SuperFAISS.A.PooledCrossDevice",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSPooledCrossDeviceTest::RunTest(const FString& Parameters)
{
	using namespace superfaiss;

	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	constexpr int32 Count = 300;
	constexpr int32 Dims = 48;
	const TArray<float> Rows = CompositionRows(Count, Dims, 0xB00Dull);

	USuperFAISSVectorBank* Bank = NewObject<USuperFAISSVectorBank>();
	FString Error;
	if (!TestTrue(TEXT("int8 bank built"), Bank->InitFromSource(Rows, Count, Dims,
			ESuperFAISSBankMetric::Dot, ESuperFAISSBankQuantization::Int8, {},
			TEXT("pooled-xd-test"), Error)))
	{
		return true;
	}

	const TArray<int32> Members = {2, 17, 41, 108, 260};
	const TArray<int32> EqualWeights = {7, 7, 7, 7, 7};
	const TArray<int32> MixedWeights = {1, 4, 2, 9, 3};

	// The subsystem payload byte-equals the core operator's product on the same view.
	FSuperFAISSCrossDeviceQuery Pooled;
	TestTrue(TEXT("pooled query built"),
		Subsystem->MakeCentroidQueryCrossDevice(Bank, Members, {}, Pooled));
	TestEqual(TEXT("payload dims"), Pooled.Dims, Dims);
	TestEqual(TEXT("payload padded size"), Pooled.ImageQ8.Num(), Pooled.PaddedDims);
	{
		const BankView View = Bank->GetBankView();
		TArray<int8, TAlignedHeapAllocator<16>> CoreImage;
		CoreImage.SetNumZeroed(View.paddedDims);
		double CoreScale = 0.0;
		int64 CoreSqSum = 0;
		TestTrue(TEXT("core operator"),
			MakeCentroidCrossDevice(View, Members.GetData(), Members.Num(), nullptr,
				nullptr, CoreImage.GetData(), &CoreScale, &CoreSqSum) == Status::Ok);
		TestTrue(TEXT("image byte-equals core"),
			FMemory::Memcmp(Pooled.ImageQ8.GetData(), CoreImage.GetData(),
				View.paddedDims) == 0);
		TestTrue(TEXT("scale bit-equals core"), Pooled.Scale == CoreScale);
		TestEqual(TEXT("self-dot equals core"), Pooled.SqSum, CoreSqSum);

		// The executed query IS the payload: subsystem hits bit-equal core QueryXd.
		TArray<FSuperFAISSHit> Hits;
		TestTrue(TEXT("pooled query executes"),
			Subsystem->QueryPooledCrossDevice(Bank, Pooled, 8, Hits));
		XdQuery Xd;
		Xd.q8 = CoreImage.GetData();
		Xd.scale = CoreScale;
		Xd.sqSum = CoreSqSum;
		QueryParams Params;
		Params.k = 8;
		Params.exactness = Exactness::CrossDevice;
		Workspace Ws;
		Hit CoreHits[8];
		int32_t CoreN = 0;
		TestTrue(TEXT("core QueryXd"),
			QueryXd(View, Xd, Params, Ws, CoreHits, &CoreN) == Status::Ok);
		TestEqual(TEXT("hit count"), Hits.Num(), (int32)CoreN);
		for (int32 i = 0; i < FMath::Min(Hits.Num(), (int32)CoreN); ++i)
		{
			TestTrue(FString::Printf(TEXT("hit %d bit-equal"), i),
				Hits[i].Index == CoreHits[i].index && Hits[i].Score == CoreHits[i].score);
		}
	}

	// All-equal weights bit-equal unweighted (the common factor cancels under
	// symmetric quantization); mixed weights produce a well-formed payload.
	{
		FSuperFAISSCrossDeviceQuery Weighted;
		TestTrue(TEXT("equal-weight pool"),
			Subsystem->MakeCentroidQueryCrossDevice(Bank, Members, EqualWeights, Weighted));
		TestTrue(TEXT("weighted image bit-equal"),
			Weighted.ImageQ8 == Pooled.ImageQ8);
		TestTrue(TEXT("weighted scale bit-equal"), Weighted.Scale == Pooled.Scale);
		TestEqual(TEXT("weighted self-dot equal"), Weighted.SqSum, Pooled.SqSum);

		FSuperFAISSCrossDeviceQuery Mixed;
		TestTrue(TEXT("mixed-weight pool"),
			Subsystem->MakeCentroidQueryCrossDevice(Bank, Members, MixedWeights, Mixed));
		TestEqual(TEXT("mixed payload sized"), Mixed.ImageQ8.Num(), Mixed.PaddedDims);
	}

	// Defined rejections: f32 bank (CrossDevice is int8-only), empty selection,
	// mismatched weights, and a payload executed against the wrong bank shape.
	{
		USuperFAISSVectorBank* F32 =
			CompositionBank(*this, Rows, Count, Dims, ESuperFAISSBankMetric::Dot);
		FSuperFAISSCrossDeviceQuery Rejected;
		if (F32 != nullptr)
		{
			TestFalse(TEXT("f32 bank rejected"),
				Subsystem->MakeCentroidQueryCrossDevice(F32, Members, {}, Rejected));
			TArray<FSuperFAISSHit> Hits;
			TestFalse(TEXT("f32 execution rejected"),
				Subsystem->QueryPooledCrossDevice(F32, Pooled, 4, Hits));
		}
		TestFalse(TEXT("empty selection rejected"),
			Subsystem->MakeCentroidQueryCrossDevice(Bank, {}, {}, Rejected));
		TestFalse(TEXT("weight count mismatch rejected"),
			Subsystem->MakeCentroidQueryCrossDevice(Bank, Members, {1, 2}, Rejected));
		TArray<FSuperFAISSHit> Hits;
		FSuperFAISSCrossDeviceQuery Empty;
		TestFalse(TEXT("empty payload rejected"),
			Subsystem->QueryPooledCrossDevice(Bank, Empty, 4, Hits));
	}

	return true;
}

// Review S2/M1 / Japp S-2 — the pooled-payload trust boundary at the plugin
// surface: no field of a caller-authored (or hand-edited, or corrupted-asset)
// FSuperFAISSCrossDeviceQuery may make scores or rankings ill-defined or silently
// wrong. A non-finite scale poisons Dot/L2 scores with NaN; a lying self-dot
// silently corrupts L2 rankings. Each is a defined rejection — the honest
// pipeline's payload still executes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSPooledCrossDeviceAdversarialTest,
	"SuperFAISS.A.PooledCrossDeviceAdversarial",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSPooledCrossDeviceAdversarialTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	constexpr int32 Count = 120;
	constexpr int32 Dims = 32;
	const TArray<float> Rows = CompositionRows(Count, Dims, 0xADEull);
	USuperFAISSVectorBank* Bank = NewObject<USuperFAISSVectorBank>();
	FString Error;
	if (!TestTrue(TEXT("int8 bank built"), Bank->InitFromSource(Rows, Count, Dims,
			ESuperFAISSBankMetric::Dot, ESuperFAISSBankQuantization::Int8, {},
			TEXT("pooled-adv-test"), Error)))
	{
		return true;
	}

	const TArray<int32> Members = {1, 7, 40, 99};
	FSuperFAISSCrossDeviceQuery Honest;
	TestTrue(TEXT("honest payload built"),
		Subsystem->MakeCentroidQueryCrossDevice(Bank, Members, {}, Honest));
	TArray<FSuperFAISSHit> Hits;
	TestTrue(TEXT("honest payload executes"),
		Subsystem->QueryPooledCrossDevice(Bank, Honest, 5, Hits));

	// Non-finite and negative scales: each a defined rejection.
	FSuperFAISSCrossDeviceQuery Adv = Honest;
	Adv.Scale = std::numeric_limits<double>::infinity();
	TestFalse(TEXT("+inf scale rejected"),
		Subsystem->QueryPooledCrossDevice(Bank, Adv, 5, Hits));
	Adv.Scale = std::numeric_limits<double>::quiet_NaN();
	TestFalse(TEXT("NaN scale rejected"),
		Subsystem->QueryPooledCrossDevice(Bank, Adv, 5, Hits));
	Adv.Scale = -1.0;
	TestFalse(TEXT("negative scale rejected"),
		Subsystem->QueryPooledCrossDevice(Bank, Adv, 5, Hits));

	// A lying self-dot: too high, too low, zero-on-nonzero-image. The self-dot
	// must be the image's own — a desynced payload is rejected, never repaired.
	Adv = Honest;
	Adv.SqSum = Honest.SqSum + 1;
	TestFalse(TEXT("inflated self-dot rejected"),
		Subsystem->QueryPooledCrossDevice(Bank, Adv, 5, Hits));
	Adv.SqSum = Honest.SqSum > 0 ? Honest.SqSum - 1 : 1;
	TestFalse(TEXT("deflated self-dot rejected"),
		Subsystem->QueryPooledCrossDevice(Bank, Adv, 5, Hits));
	Adv.SqSum = 0;
	TestFalse(TEXT("zero self-dot on a nonzero image rejected"),
		Subsystem->QueryPooledCrossDevice(Bank, Adv, 5, Hits));

	// The honest payload still executes after the adversarial sweep.
	TestTrue(TEXT("honest payload unaffected"),
		Subsystem->QueryPooledCrossDevice(Bank, Honest, 5, Hits));

	return true;
}

// T-V2.5-U1 (plan section 22, test design section 7): the V2.5 analytics BP entry points
// wrap the vendored core, so each returns the SAME scalar a direct core call returns on
// the same rows (INV). The UE-compiled core is the same code the standalone core suite
// pins; this proves the surface's plumbing — row-index passing, scratch, and the Status
// -> bool mapping — is faithful, and that an f32 bank and a -128 payload are rejected.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSAnalyticsSurfaceTest,
	"SuperFAISS.A.AnalyticsSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSAnalyticsSurfaceTest::RunTest(const FString& Parameters)
{
	using namespace superfaiss;

	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	auto BitEqual = [](float A, float B)
	{
		uint32 a, b;
		FMemory::Memcpy(&a, &A, 4);
		FMemory::Memcpy(&b, &B, 4);
		return a == b;
	};

	constexpr int32 Count = 96;
	constexpr int32 Dims = 48;
	const TArray<float> Rows = CompositionRows(Count, Dims, 0xA0A11CE5ull);
	const TArray<float> TargetRows = CompositionRows(Count, Dims, 0x7A46E7ull);
	const TArray<int32> SelA = {2, 5, 9, 40, 70};
	const TArray<int32> SelB = {3, 8, 15, 50, 80};

	const ESuperFAISSBankMetric Metrics[] = {
		ESuperFAISSBankMetric::Dot, ESuperFAISSBankMetric::Cosine, ESuperFAISSBankMetric::L2};

	for (const ESuperFAISSBankMetric Metric : Metrics)
	{
		const FString Tag = FString::Printf(TEXT("metric %d"), static_cast<int32>(Metric));

		USuperFAISSVectorBank* Bank = NewObject<USuperFAISSVectorBank>();
		FString Error;
		if (!TestTrue(*FString::Printf(TEXT("%s: source bank"), *Tag),
				Bank->InitFromSource(Rows, Count, Dims, Metric,
					ESuperFAISSBankQuantization::Int8, {}, TEXT("analytics-src"), Error)))
		{
			return true;
		}
		USuperFAISSVectorBank* Target = NewObject<USuperFAISSVectorBank>();
		if (!TestTrue(*FString::Printf(TEXT("%s: target bank"), *Tag),
				Target->InitFromSource(TargetRows, Count, Dims, Metric,
					ESuperFAISSBankQuantization::Int8, {}, TEXT("analytics-tgt"), Error)))
		{
			return true;
		}
		const BankView View = Bank->GetBankView();
		const BankView TargetView = Target->GetBankView();
		const superfaiss::Metric CoreMetric = static_cast<superfaiss::Metric>(Metric);

		// --- Set-to-set distance == core CentroidDistanceCrossDevice ---
		{
			TArray<int8, TAlignedHeapAllocator<16>> ScratchA;
			TArray<int8, TAlignedHeapAllocator<16>> ScratchB;
			ScratchA.SetNumZeroed(View.paddedDims);
			ScratchB.SetNumZeroed(View.paddedDims);
			float CoreDistance = 0.0f;
			const Status s = CentroidDistanceCrossDevice(
				View, SelA.GetData(), SelA.Num(), nullptr, nullptr,
				View, SelB.GetData(), SelB.Num(), nullptr, nullptr,
				CoreMetric, ScratchA.GetData(), ScratchB.GetData(), &CoreDistance);
			float PluginDistance = 0.0f;
			const bool bOk = Subsystem->SetToSetDistanceCrossDevice(
				Bank, SelA, {}, Bank, SelB, {}, Metric, PluginDistance);
			TestTrue(*FString::Printf(TEXT("%s: core set-to-set ok"), *Tag), s == Status::Ok);
			TestTrue(*FString::Printf(TEXT("%s: plugin set-to-set ok"), *Tag), bOk);
			TestTrue(*FString::Printf(TEXT("%s: set-to-set bit-equals core"), *Tag),
				BitEqual(CoreDistance, PluginDistance));
		}

		// --- Spread == core SpreadCrossDevice, both reductions ---
		{
			const ESuperFAISSReduce Reductions[] = {
				ESuperFAISSReduce::Mean, ESuperFAISSReduce::Max};
			for (const ESuperFAISSReduce Reduce : Reductions)
			{
				TArray<int8, TAlignedHeapAllocator<16>> Scratch;
				Scratch.SetNumZeroed(View.paddedDims);
				float CoreValue = 0.0f;
				const Status s = SpreadCrossDevice(View, SelA.GetData(), SelA.Num(), nullptr,
					static_cast<superfaiss::Reduce>(Reduce), Scratch.GetData(), &CoreValue);
				float PluginValue = 0.0f;
				const bool bOk = Subsystem->BankSpreadCrossDevice(Bank, SelA, Reduce, PluginValue);
				TestTrue(*FString::Printf(TEXT("%s: core spread ok"), *Tag), s == Status::Ok);
				TestTrue(*FString::Printf(TEXT("%s: plugin spread ok"), *Tag), bOk);
				TestTrue(*FString::Printf(TEXT("%s: spread bit-equals core"), *Tag),
					BitEqual(CoreValue, PluginValue));
			}
		}

		// --- Mean/Max nearest-neighbour == core Mean/MaxNNCrossDevice ---
		{
			TArray<XdQuery> QueryScratch;
			TArray<Hit> HitScratch;
			TArray<int32> CountScratch;
			QueryScratch.SetNumUninitialized(View.count);
			HitScratch.SetNumUninitialized(View.count);
			CountScratch.SetNumUninitialized(View.count);

			Workspace WsMean;
			float CoreMean = 0.0f;
			const Status sMean = MeanNNCrossDevice(View, nullptr, TargetView, nullptr,
				QueryScratch.GetData(), HitScratch.GetData(), CountScratch.GetData(),
				WsMean, &CoreMean);
			float PluginMean = 0.0f;
			const bool bMeanOk =
				Subsystem->MeanNearestNeighborCrossDevice(Bank, Target, PluginMean);
			TestTrue(*FString::Printf(TEXT("%s: core meanNN ok"), *Tag), sMean == Status::Ok);
			TestTrue(*FString::Printf(TEXT("%s: plugin meanNN ok"), *Tag), bMeanOk);
			TestTrue(*FString::Printf(TEXT("%s: meanNN bit-equals core"), *Tag),
				BitEqual(CoreMean, PluginMean));

			Workspace WsMax;
			float CoreMax = 0.0f;
			const Status sMax = MaxNNCrossDevice(View, nullptr, TargetView, nullptr,
				QueryScratch.GetData(), HitScratch.GetData(), CountScratch.GetData(),
				WsMax, &CoreMax);
			float PluginMax = 0.0f;
			const bool bMaxOk =
				Subsystem->MaxNearestNeighborCrossDevice(Bank, Target, PluginMax);
			TestTrue(*FString::Printf(TEXT("%s: core maxNN ok"), *Tag), sMax == Status::Ok);
			TestTrue(*FString::Printf(TEXT("%s: plugin maxNN ok"), *Tag), bMaxOk);
			TestTrue(*FString::Printf(TEXT("%s: maxNN bit-equals core"), *Tag),
				BitEqual(CoreMax, PluginMax));
		}
	}

	// --- ScoreXdPair pair primitive == core, on Dot/Cosine/L2; + the D-V2-13 guard ---
	{
		USuperFAISSVectorBank* Bank = NewObject<USuperFAISSVectorBank>();
		FString Error;
		if (!TestTrue(TEXT("pair bank"), Bank->InitFromSource(Rows, Count, Dims,
				ESuperFAISSBankMetric::Dot, ESuperFAISSBankQuantization::Int8, {},
				TEXT("analytics-pair"), Error)))
		{
			return true;
		}
		FSuperFAISSCrossDeviceQuery PayloadA;
		FSuperFAISSCrossDeviceQuery PayloadB;
		TestTrue(TEXT("payload A built"),
			Subsystem->MakeCentroidQueryCrossDevice(Bank, SelA, {}, PayloadA));
		TestTrue(TEXT("payload B built"),
			Subsystem->MakeCentroidQueryCrossDevice(Bank, SelB, {}, PayloadB));

		if (PayloadA.PaddedDims > 0 && PayloadA.PaddedDims == PayloadB.PaddedDims)
		{
			TArray<int8, TAlignedHeapAllocator<16>> ImageA;
			TArray<int8, TAlignedHeapAllocator<16>> ImageB;
			ImageA.SetNumZeroed(PayloadA.PaddedDims);
			ImageB.SetNumZeroed(PayloadB.PaddedDims);
			FMemory::Memcpy(ImageA.GetData(), PayloadA.ImageQ8.GetData(), PayloadA.PaddedDims);
			FMemory::Memcpy(ImageB.GetData(), PayloadB.ImageQ8.GetData(), PayloadB.PaddedDims);
			XdQuery Xa;
			Xa.q8 = ImageA.GetData();
			Xa.scale = PayloadA.Scale;
			Xa.sqSum = PayloadA.SqSum;
			XdQuery Xb;
			Xb.q8 = ImageB.GetData();
			Xb.scale = PayloadB.Scale;
			Xb.sqSum = PayloadB.SqSum;

			const ESuperFAISSBankMetric PairMetrics[] = {ESuperFAISSBankMetric::Dot,
				ESuperFAISSBankMetric::Cosine, ESuperFAISSBankMetric::L2};
			for (const ESuperFAISSBankMetric Metric : PairMetrics)
			{
				float CoreScore = 0.0f;
				const Status s = ScoreXdPair(Xa, Xb, PayloadA.PaddedDims,
					static_cast<superfaiss::Metric>(Metric), &CoreScore);
				float PluginScore = 0.0f;
				const bool bOk =
					Subsystem->ScoreCrossDeviceQueryPair(PayloadA, PayloadB, Metric, PluginScore);
				TestTrue(TEXT("core pair ok"), s == Status::Ok);
				TestTrue(TEXT("plugin pair ok"), bOk);
				TestTrue(TEXT("pair bit-equals core"), BitEqual(CoreScore, PluginScore));
			}
		}

		// D-V2-13: a hand-forged payload carrying INT8_MIN (-128) with a matching
		// self-dot passes IsPayloadValid, and is rejected by the public boundary's
		// -128 guard (the +-127 premise enforced, not merely asserted).
		FSuperFAISSCrossDeviceQuery Neg = PayloadA;
		if (Neg.ImageQ8.Num() > 0)
		{
			const int8 Prev = static_cast<int8>(Neg.ImageQ8[0]);
			Neg.ImageQ8[0] = static_cast<uint8>(static_cast<int8>(-128));
			Neg.SqSum += static_cast<int64>(-128) * -128 - static_cast<int64>(Prev) * Prev;
			TestTrue(TEXT("forged -128 payload is self-consistent"), Neg.IsPayloadValid());
			float Rejected = 0.0f;
			TestFalse(TEXT("-128 payload rejected (D-V2-13)"),
				Subsystem->ScoreCrossDeviceQueryPair(Neg, PayloadB,
					ESuperFAISSBankMetric::Dot, Rejected));
		}
		float Valid = 0.0f;
		TestTrue(TEXT("valid pair still scores after the forged one"),
			Subsystem->ScoreCrossDeviceQueryPair(PayloadA, PayloadB,
				ESuperFAISSBankMetric::Dot, Valid));
	}

	// --- Projection report == core, and its separation finds a planted split ---
	{
		USuperFAISSVectorBank* Bank = NewObject<USuperFAISSVectorBank>();
		FString Error;
		if (TestTrue(TEXT("projection bank"), Bank->InitFromSource(Rows, Count, Dims,
				ESuperFAISSBankMetric::Dot, ESuperFAISSBankQuantization::Int8, {},
				TEXT("analytics-proj"), Error)))
		{
			const BankView View = Bank->GetBankView();
			TArray<float> VectorA;
			TArray<float> VectorB;
			VectorA.SetNumUninitialized(Dims);
			VectorB.SetNumUninitialized(Dims);
			for (int32 J = 0; J < Dims; ++J)
			{
				VectorA[J] = Rows[2 * Dims + J];
				VectorB[J] = Rows[9 * Dims + J];
			}
			const TArray<int32> GroupA = {0, 1, 2, 3, 4, 5, 6, 7};

			// Core reference: build the padded unit direction, then ProjectionReport.
			TArray<float, TAlignedHeapAllocator<16>> PaddedA;
			TArray<float, TAlignedHeapAllocator<16>> PaddedB;
			TArray<float, TAlignedHeapAllocator<16>> PaddedDir;
			PaddedA.SetNumZeroed(View.paddedDims);
			PaddedB.SetNumZeroed(View.paddedDims);
			PaddedDir.SetNumZeroed(View.paddedDims);
			FMemory::Memcpy(PaddedA.GetData(), VectorA.GetData(), Dims * sizeof(float));
			FMemory::Memcpy(PaddedB.GetData(), VectorB.GetData(), Dims * sizeof(float));
			const Status sDir = MakeDirection(PaddedA.GetData(), PaddedB.GetData(), Dims,
				View.paddedDims, PaddedDir.GetData());
			TestTrue(TEXT("core direction ok"), sDir == Status::Ok);

			TArray<uint32> GroupBits;
			GroupBits.SetNumZeroed((View.count + 31) / 32);
			for (const int32 Index : GroupA)
			{
				GroupBits[Index >> 5] |= (1u << (Index & 31));
			}
			TArray<float> CoreProjections;
			CoreProjections.SetNumUninitialized(View.count);
			float CoreSeparation = 0.0f;
			const Status sProj = superfaiss::ProjectionReport(View, PaddedDir.GetData(),
				GroupBits.GetData(), CoreProjections.GetData(), &CoreSeparation);
			TestTrue(TEXT("core projection ok"), sProj == Status::Ok);

			TArray<float> PluginProjections;
			float PluginSeparation = 0.0f;
			const bool bOk = Subsystem->ProjectionReport(Bank, VectorA, VectorB, GroupA,
				PluginProjections, PluginSeparation);
			TestTrue(TEXT("plugin projection ok"), bOk);
			TestEqual(TEXT("projection count"), PluginProjections.Num(), CoreProjections.Num());
			bool bAllEqual = PluginProjections.Num() == CoreProjections.Num();
			for (int32 i = 0; i < PluginProjections.Num() && i < CoreProjections.Num(); ++i)
			{
				bAllEqual &= BitEqual(CoreProjections[i], PluginProjections[i]);
			}
			TestTrue(TEXT("projections bit-equal core"), bAllEqual);
			TestTrue(TEXT("separation bit-equals core"),
				BitEqual(CoreSeparation, PluginSeparation));

			// A GroupA covering every row (empty complement) is a defined rejection (F3).
			TArray<int32> AllRows;
			AllRows.SetNumUninitialized(Count);
			for (int32 i = 0; i < Count; ++i)
			{
				AllRows[i] = i;
			}
			TArray<float> Dropped;
			float DroppedSep = 0.0f;
			TestFalse(TEXT("group over all rows rejected"),
				Subsystem->ProjectionReport(Bank, VectorA, VectorB, AllRows, Dropped, DroppedSep));
			TestEqual(TEXT("no partial projections on rejection"), Dropped.Num(), 0);

			// A == B has no direction.
			TArray<float> NoDir;
			float NoSep = 0.0f;
			TestFalse(TEXT("A==B rejected"),
				Subsystem->ProjectionReport(Bank, VectorA, VectorA, GroupA, NoDir, NoSep));
		}
	}

	// --- Rejections: an f32 bank is refused by every cross-device analytics entry ---
	{
		USuperFAISSVectorBank* F32Bank = NewObject<USuperFAISSVectorBank>();
		FString Error;
		if (TestTrue(TEXT("f32 bank"), F32Bank->InitFromSource(Rows, Count, Dims,
				ESuperFAISSBankMetric::Dot, ESuperFAISSBankQuantization::Float32, {},
				TEXT("analytics-f32"), Error)))
		{
			float Out = 0.0f;
			TestFalse(TEXT("f32 spread refused"),
				Subsystem->BankSpreadCrossDevice(F32Bank, SelA, ESuperFAISSReduce::Mean, Out));
			TestFalse(TEXT("f32 set-to-set refused"),
				Subsystem->SetToSetDistanceCrossDevice(F32Bank, SelA, {}, F32Bank, SelB, {},
					ESuperFAISSBankMetric::Dot, Out));
			TestFalse(TEXT("f32 meanNN refused"),
				Subsystem->MeanNearestNeighborCrossDevice(F32Bank, F32Bank, Out));

			// Empty selections are refused up front.
			TestFalse(TEXT("empty spread selection refused"),
				Subsystem->BankSpreadCrossDevice(F32Bank, {}, ESuperFAISSReduce::Mean, Out));
		}
	}

	return true;
}

// T-V2.5-U2 (plan section 22, test design section 7): the scratch overloads drive the
// analytics over a live scratch Snapshot() with the snapshot's tombstones OR'd into the
// exclusion set. Each returns the core snapshot result (INV, compared against a direct
// core call over the identical snapshot), and a tombstoned row is excluded from both the
// pool and the reduction. The concurrency/TSan proof of the snapshot lives core-side
// (T-V2.5-10); this is the plugin closure — the surface computes over a real snapshot.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSAnalyticsScratchTest,
	"SuperFAISS.A.AnalyticsScratch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSAnalyticsScratchTest::RunTest(const FString& Parameters)
{
	using namespace superfaiss;

	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	auto BitEqual = [](float A, float B)
	{
		uint32 a, b;
		FMemory::Memcpy(&a, &A, 4);
		FMemory::Memcpy(&b, &B, 4);
		return a == b;
	};

	constexpr int32 Dims = 32;
	constexpr int32 Capacity = 64;
	const TArray<float> Feed = CompositionRows(48, Dims, 0x5C9A7C4ull);

	USuperFAISSScratchBank* Scratch = NewObject<USuperFAISSScratchBank>();
	if (!TestTrue(TEXT("scratch init"), Scratch->Init(Capacity, Dims,
			ESuperFAISSBankMetric::Dot, ESuperFAISSBankQuantization::Int8)))
	{
		return true;
	}
	// Append 24 rows; one deliberate outlier we then remove, so excluding the tombstone
	// provably moves the statistic (the exclusion is not a no-op on this fixture).
	int32 Appended = 0;
	int32 OutlierIndex = -1;
	for (int32 R = 0; R < 24; ++R)
	{
		TArray<float> Row;
		Row.SetNumUninitialized(Dims);
		for (int32 J = 0; J < Dims; ++J)
		{
			Row[J] = Feed[R * Dims + J];
		}
		int32 Index = -1;
		if (R == 12)
		{
			for (float& V : Row)
			{
				V *= 40.0f; // a far-off row: removing it changes the dispersion
			}
		}
		TestTrue(TEXT("scratch append"), Scratch->Append(Row, Index));
		if (R == 12)
		{
			OutlierIndex = Index;
		}
		++Appended;
	}
	TestTrue(TEXT("outlier removed"), Scratch->Remove(OutlierIndex));

	// A baked int8 target bank for the divergence overloads (same dims/metric).
	const TArray<float> TargetRows = CompositionRows(40, Dims, 0x9E3779B9ull);
	USuperFAISSVectorBank* Target = NewObject<USuperFAISSVectorBank>();
	FString Error;
	if (!TestTrue(TEXT("target bank"), Target->InitFromSource(TargetRows, 40, Dims,
			ESuperFAISSBankMetric::Dot, ESuperFAISSBankQuantization::Int8, {},
			TEXT("scratch-target"), Error)))
	{
		return true;
	}
	const BankView TargetView = Target->GetBankView();
	const TArray<int32> TargetSel = {1, 4, 9, 20, 33};

	// Reference: snapshot the scratch bank exactly as the plugin does, then run the core
	// operator over the identical bytes. `bExclude` toggles the tombstones so we can prove
	// the removed row actually drops out.
	auto CoreSpreadOverSnapshot = [&](superfaiss::Reduce Reduce, bool bExclude, float& Out)
	{
		if (!Scratch->TryPin())
		{
			return false;
		}
		bool bOk = false;
		{
			BankView View;
			TArray<uint32> Tombstones;
			Tombstones.SetNumZeroed(ScratchBank::TombstoneWords(Scratch->Core().Capacity()));
			if (Scratch->Core().Snapshot(&View, Tombstones.GetData()) == Status::Ok &&
				View.count > 0)
			{
				TArray<int32> RowIndices;
				RowIndices.SetNumUninitialized(View.count);
				for (int32 i = 0; i < View.count; ++i)
				{
					RowIndices[i] = i;
				}
				TArray<int8, TAlignedHeapAllocator<16>> CentroidScratch;
				CentroidScratch.SetNumZeroed(View.paddedDims);
				bOk = SpreadCrossDevice(View, RowIndices.GetData(), View.count,
					bExclude ? Tombstones.GetData() : nullptr, Reduce,
					CentroidScratch.GetData(), &Out) == Status::Ok;
			}
		}
		Scratch->Unpin();
		return bOk;
	};

	// --- Spread over the snapshot: plugin == core-with-tombstones, and != core-without ---
	{
		float PluginMean = 0.0f;
		TestTrue(TEXT("scratch spread mean ok"),
			Subsystem->BankSpreadCrossDeviceScratch(Scratch, ESuperFAISSReduce::Mean, PluginMean));
		float CoreWith = 0.0f;
		TestTrue(TEXT("core spread (tombstoned) ok"),
			CoreSpreadOverSnapshot(superfaiss::Reduce::Mean, true, CoreWith));
		TestTrue(TEXT("scratch spread bit-equals core snapshot"), BitEqual(PluginMean, CoreWith));

		float CoreWithout = 0.0f;
		TestTrue(TEXT("core spread (no exclusion) ok"),
			CoreSpreadOverSnapshot(superfaiss::Reduce::Mean, false, CoreWithout));
		TestFalse(TEXT("tombstone actually excluded (value moved)"),
			BitEqual(CoreWith, CoreWithout));
	}

	// --- Mean nearest-neighbour from the scratch snapshot to the baked target ---
	{
		float Plugin = 0.0f;
		TestTrue(TEXT("scratch meanNN ok"),
			Subsystem->MeanNearestNeighborCrossDeviceScratch(Scratch, Target, Plugin));

		float Core = 0.0f;
		bool bRefOk = false;
		if (Scratch->TryPin())
		{
			BankView View;
			TArray<uint32> Tombstones;
			Tombstones.SetNumZeroed(ScratchBank::TombstoneWords(Scratch->Core().Capacity()));
			if (Scratch->Core().Snapshot(&View, Tombstones.GetData()) == Status::Ok &&
				View.count > 0)
			{
				TArray<XdQuery> Q;
				TArray<Hit> H;
				TArray<int32> C;
				Q.SetNumUninitialized(View.count);
				H.SetNumUninitialized(View.count);
				C.SetNumUninitialized(View.count);
				Workspace Ws;
				bRefOk = MeanNNCrossDevice(View, Tombstones.GetData(), TargetView, nullptr,
					Q.GetData(), H.GetData(), C.GetData(), Ws, &Core) == Status::Ok;
			}
			Scratch->Unpin();
		}
		TestTrue(TEXT("core meanNN over snapshot ok"), bRefOk);
		TestTrue(TEXT("scratch meanNN bit-equals core snapshot"), BitEqual(Plugin, Core));
	}

	// --- Set-to-set between the scratch snapshot and the baked target ---
	{
		float Plugin = 0.0f;
		TestTrue(TEXT("scratch set-to-set ok"),
			Subsystem->SetToSetDistanceCrossDeviceScratch(Scratch, Target, TargetSel, {},
				ESuperFAISSBankMetric::Dot, Plugin));

		float Core = 0.0f;
		bool bRefOk = false;
		if (Scratch->TryPin())
		{
			BankView View;
			TArray<uint32> Tombstones;
			Tombstones.SetNumZeroed(ScratchBank::TombstoneWords(Scratch->Core().Capacity()));
			if (Scratch->Core().Snapshot(&View, Tombstones.GetData()) == Status::Ok &&
				View.count > 0)
			{
				TArray<int32> RowIndices;
				RowIndices.SetNumUninitialized(View.count);
				for (int32 i = 0; i < View.count; ++i)
				{
					RowIndices[i] = i;
				}
				TArray<int8, TAlignedHeapAllocator<16>> ScratchA;
				TArray<int8, TAlignedHeapAllocator<16>> ScratchB;
				ScratchA.SetNumZeroed(View.paddedDims);
				ScratchB.SetNumZeroed(TargetView.paddedDims);
				bRefOk = CentroidDistanceCrossDevice(
					View, RowIndices.GetData(), View.count, nullptr, Tombstones.GetData(),
					TargetView, TargetSel.GetData(), TargetSel.Num(), nullptr, nullptr,
					Metric::Dot, ScratchA.GetData(), ScratchB.GetData(), &Core) == Status::Ok;
			}
			Scratch->Unpin();
		}
		TestTrue(TEXT("core set-to-set over snapshot ok"), bRefOk);
		TestTrue(TEXT("scratch set-to-set bit-equals core snapshot"), BitEqual(Plugin, Core));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

