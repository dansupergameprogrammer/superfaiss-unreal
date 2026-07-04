// Groups E and F against the shipped demo bank (plan §12; E-group in the
// absolute-guard form settled by Dan 2026-07-03 — DecisionLog).

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "SuperFAISSSubsystem.h"
#include "SuperFAISSVectorBank.h"

namespace
{
	USuperFAISSVectorBank* LoadDemoBank(FAutomationTestBase& Test)
	{
		USuperFAISSVectorBank* Bank = LoadObject<USuperFAISSVectorBank>(
			nullptr, TEXT("/SuperFAISSUnreal/Demo/DemoBank.DemoBank"));
		Test.TestNotNull(TEXT("demo bank loads"), Bank);
		if (Bank)
		{
			Test.TestTrue(TEXT("demo bank validates"), Bank->IsValid());
		}
		return Bank && Bank->IsValid() ? Bank : nullptr;
	}

	int32 FindWord(const USuperFAISSVectorBank* Bank, FName Word)
	{
		return Bank->Ids.IndexOfByKey(Word);
	}

	// The demo bank stores normalized rows; a word's own row is its query vector.
	TArray<float> WordQuery(const USuperFAISSVectorBank* Bank, int32 Row)
	{
		using namespace superfaiss;
		const BankView View = Bank->GetBankView();
		const int8* RowData = static_cast<const int8*>(View.rows) +
			static_cast<int64>(Row) * View.paddedDims;
		const float Scale = View.scales[Row];
		TArray<float> Query;
		Query.SetNumUninitialized(Bank->Dims);
		for (int32 i = 0; i < Bank->Dims; ++i)
		{
			Query[i] = Scale * RowData[i];
		}
		return Query;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSDemoGoldenTest,
	"SuperFAISS.F.DemoBankGoldenQuery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSDemoGoldenTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	USuperFAISSVectorBank* Bank = LoadDemoBank(*this);
	if (!Subsystem || !Bank)
	{
		return true;
	}
	TestEqual(TEXT("bank size"), Bank->Count, 40000);

	const int32 WizardRow = FindWord(Bank, FName("wizard"));
	TestTrue(TEXT("'wizard' present"), WizardRow >= 0);
	if (WizardRow < 0)
	{
		return true;
	}

	TArray<uint32> Exclude;
	Exclude.SetNumZeroed((Bank->Count + 31) / 32);
	Exclude[WizardRow >> 5] |= 1u << (WizardRow & 31);

	FSuperFAISSQueryArgs Args;
	Args.K = 5;
	Args.ExcludeBits = Exclude;

	TArray<FSuperFAISSHit> Hits;
	TestTrue(TEXT("query ok"),
		Subsystem->QuerySync(Bank, WordQuery(Bank, WizardRow), Args, Hits));
	TestEqual(TEXT("k hits"), Hits.Num(), 5);

	FString Neighbors;
	for (const FSuperFAISSHit& H : Hits)
	{
		Neighbors += H.Id.ToString() + TEXT(" ");
	}
	UE_LOG(LogTemp, Display, TEXT("F1 'wizard' -> %s"), *Neighbors);

	// Golden pins (semantic sanity on the shipped bank).
	if (Hits.Num() == 5)
	{
		TestEqual(TEXT("top neighbor"), Hits[0].Id, FName("magician"));
		const bool bHasSorcerer = Hits.ContainsByPredicate(
			[](const FSuperFAISSHit& H) { return H.Id == FName("sorcerer"); });
		TestTrue(TEXT("'sorcerer' in top 5"), bHasSorcerer);
	}

	// Determinism on the shipped asset: repeat query is bit-identical.
	TArray<FSuperFAISSHit> Again;
	TestTrue(TEXT("repeat ok"),
		Subsystem->QuerySync(Bank, WordQuery(Bank, WizardRow), Args, Again));
	TestEqual(TEXT("repeat count"), Again.Num(), Hits.Num());
	for (int32 i = 0; i < FMath::Min(Again.Num(), Hits.Num()); ++i)
	{
		TestEqual(TEXT("repeat index"), Again[i].Index, Hits[i].Index);
		TestEqual(TEXT("repeat score"), Again[i].Score, Hits[i].Score);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSDemoPerfTest,
	"SuperFAISS.E.AbsoluteGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSDemoPerfTest::RunTest(const FString& Parameters)
{
	// E-group, absolute-guard form (DecisionLog 2026-07-03):
	//   E1 — single-query ceiling on the shipped demo bank (40k x 100 int8): 1.5 ms.
	//        Calibration measures the PARALLEL scan path (superfaiss.ParallelScan auto;
	//        ~70 chunks): 0.13 ms quiet (~11x headroom). Batch runs the parallel
	//        chunk-outer pair-kernel path: 0.06 ms/query quiet.
	//   E2 — never-worse: batch per-query <= 1.25x single, measured in the same run
	//        (machine-load independent). Amortization is reported, not asserted.
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	USuperFAISSVectorBank* Bank = LoadDemoBank(*this);
	if (!Subsystem || !Bank)
	{
		return true;
	}

	constexpr int32 BatchWidth = 64;
	FSuperFAISSQueryArgs Args;
	Args.K = 10;

	// Batch queries: word rows spread across the bank.
	TArray<float> Queries;
	Queries.Reserve(BatchWidth * Bank->Dims);
	for (int32 M = 0; M < BatchWidth; ++M)
	{
		Queries.Append(WordQuery(Bank, (M * 307) % Bank->Count));
	}

	TArray<FSuperFAISSHit> Hits;
	TArray<int32> Counts;

	// Warm-up.
	Subsystem->QuerySync(Bank, TConstArrayView<float>(Queries.GetData(), Bank->Dims), Args, Hits);
	Subsystem->QueryBatch(Bank, Queries, BatchWidth, Args, Hits, Counts);

	// Single: best of 5 runs of 20.
	double BestSingle = 1e300;
	for (int32 Run = 0; Run < 5; ++Run)
	{
		const double T0 = FPlatformTime::Seconds();
		for (int32 R = 0; R < 20; ++R)
		{
			Subsystem->QuerySync(Bank,
				TConstArrayView<float>(Queries.GetData() + (R % BatchWidth) * Bank->Dims, Bank->Dims),
				Args, Hits);
		}
		BestSingle = FMath::Min(BestSingle, (FPlatformTime::Seconds() - T0) / 20.0);
	}

	// Batch: best of 5.
	double BestBatch = 1e300;
	for (int32 Run = 0; Run < 5; ++Run)
	{
		const double T0 = FPlatformTime::Seconds();
		Subsystem->QueryBatch(Bank, Queries, BatchWidth, Args, Hits, Counts);
		BestBatch = FMath::Min(BestBatch, FPlatformTime::Seconds() - T0);
	}
	const double BatchPerQuery = BestBatch / BatchWidth;

	UE_LOG(LogTemp, Display,
		TEXT("E-group: single %.3f ms | batch64 per-query %.3f ms | amortization %.2fx (reported, not asserted)"),
		BestSingle * 1e3, BatchPerQuery * 1e3, BestSingle / BatchPerQuery);

	TestTrue(FString::Printf(TEXT("E1 single %.3f ms <= 1.5 ms"), BestSingle * 1e3),
		BestSingle <= 0.0015);
	TestTrue(FString::Printf(TEXT("E2 never-worse: batch per-query %.3f ms <= 1.25x single %.3f ms"),
		BatchPerQuery * 1e3, BestSingle * 1e3),
		BatchPerQuery <= BestSingle * 1.25);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
