// Subsystem-level tests: async delivery + cancellation (plan B4), pool-flat
// allocation (B5), thread-storm consistency (B7), batch ≡ singles at the API
// surface (A5).

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/ParallelFor.h"
#include "HAL/IConsoleManager.h"
#include "SuperFAISSSubsystem.h"
#include "SuperFAISSVectorBank.h"

#include "superfaiss/superfaiss.h"

namespace
{
	USuperFAISSVectorBank* MakeBank(FAutomationTestBase& Test, int32 Count, int32 Dims)
	{
		uint64 State = 0xFEEDFACE0ull;
		TArray<float> Rows;
		Rows.SetNumUninitialized(Count * Dims);
		for (float& V : Rows)
		{
			State ^= State >> 12;
			State ^= State << 25;
			State ^= State >> 27;
			V = static_cast<float>(static_cast<int64>((State * 0x2545F4914F6CDD1Dull) >> 24)) /
				static_cast<float>(1ll << 39);
		}
		USuperFAISSVectorBank* Bank = NewObject<USuperFAISSVectorBank>();
		FString Error;
		const bool bOk = Bank->InitFromSource(Rows, Count, Dims, ESuperFAISSBankMetric::Cosine,
			ESuperFAISSBankQuantization::Int8, {}, TEXT("subsystem-test"), Error);
		Test.TestTrue(FString::Printf(TEXT("bank built: %s"), *Error), bOk);
		return bOk ? Bank : nullptr;
	}

	TArray<float> MakeQuery(int32 Dims, uint64 Seed)
	{
		TArray<float> Query;
		Query.SetNumUninitialized(Dims);
		uint64 State = Seed;
		for (float& V : Query)
		{
			State ^= State >> 12;
			State ^= State << 25;
			State ^= State >> 27;
			V = static_cast<float>(static_cast<int64>((State * 0x2545F4914F6CDD1Dull) >> 24)) /
				static_cast<float>(1ll << 39);
		}
		return Query;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSSubsystemStormTest,
	"SuperFAISS.B.ThreadStormConsistency",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSSubsystemStormTest::RunTest(const FString& Parameters)
{
	// B7: 16 workers hammer QuerySync on one shared bank; every result must be
	// bit-identical to its serial twin. Also seeds B5: pool growth is checked flat
	// across the storm after warm-up.
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	USuperFAISSVectorBank* Bank = MakeBank(*this, 4000, 32);
	if (!Subsystem || !Bank)
	{
		return true;
	}

	constexpr int32 QueryVariants = 8;
	constexpr int32 Workers = 16;
	FSuperFAISSQueryArgs Args;
	Args.K = 12;

	// Serial reference results.
	TArray<TArray<FSuperFAISSHit>> Reference;
	Reference.SetNum(QueryVariants);
	for (int32 V = 0; V < QueryVariants; ++V)
	{
		TestTrue(TEXT("serial query"),
			Subsystem->QuerySync(Bank, MakeQuery(32, 100 + V), Args, Reference[V]));
	}

	// Warm-up: one full storm-shaped round, so the pool reaches the scheduler's
	// true peak concurrency before growth is snapshotted (B5's "after warm-up").
	TArray<TArray<FSuperFAISSHit>> StormResults;
	StormResults.SetNum(Workers * QueryVariants);
	auto RunStorm = [&]()
	{
		ParallelFor(Workers, [&](int32 W)
		{
			for (int32 V = 0; V < QueryVariants; ++V)
			{
				Subsystem->QuerySync(Bank, MakeQuery(32, 100 + V), Args,
					StormResults[W * QueryVariants + V]);
			}
		});
	};
	RunStorm();

	const uint64 GrowthBefore = Subsystem->GetPoolGrowthCount();
	const uint64 CoreAllocsBefore = superfaiss::AllocationCount();
	RunStorm();

	for (int32 W = 0; W < Workers; ++W)
	{
		for (int32 V = 0; V < QueryVariants; ++V)
		{
			const TArray<FSuperFAISSHit>& Got = StormResults[W * QueryVariants + V];
			const TArray<FSuperFAISSHit>& Ref = Reference[V];
			if (Got.Num() != Ref.Num())
			{
				AddError(FString::Printf(TEXT("storm count mismatch w%d v%d"), W, V));
				continue;
			}
			for (int32 i = 0; i < Got.Num(); ++i)
			{
				if (Got[i].Index != Ref[i].Index || Got[i].Score != Ref[i].Score)
				{
					AddError(FString::Printf(TEXT("storm divergence w%d v%d hit %d"), W, V, i));
					break;
				}
			}
		}
	}
	TestEqual(TEXT("checked all storm cells"), StormResults.Num(), Workers * QueryVariants);

	// B5: neither the subsystem pool nor the core allocator moved during the storm.
	TestEqual(TEXT("pool growth flat"), Subsystem->GetPoolGrowthCount(), GrowthBefore);
	TestEqual(TEXT("core allocations flat"), superfaiss::AllocationCount(), CoreAllocsBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSSubsystemBatchTest,
	"SuperFAISS.A.SubsystemBatchEquivalence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSSubsystemBatchTest::RunTest(const FString& Parameters)
{
	// A5 at the API surface: QueryBatch ≡ QuerySync per query, bit-for-bit, including
	// across the pair-kernel path and the odd-count tail.
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	USuperFAISSVectorBank* Bank = MakeBank(*this, 2500, 48);
	if (!Subsystem || !Bank)
	{
		return true;
	}

	constexpr int32 QueryCount = 7; // odd: exercises the pair loop's tail
	FSuperFAISSQueryArgs Args;
	Args.K = 9;

	TArray<float> Queries;
	Queries.Reserve(QueryCount * 48);
	for (int32 M = 0; M < QueryCount; ++M)
	{
		Queries.Append(MakeQuery(48, 500 + M));
	}

	TArray<FSuperFAISSHit> BatchHits;
	TArray<int32> BatchCounts;
	TestTrue(TEXT("batch ok"),
		Subsystem->QueryBatch(Bank, Queries, QueryCount, Args, BatchHits, BatchCounts));

	for (int32 M = 0; M < QueryCount; ++M)
	{
		TArray<FSuperFAISSHit> Single;
		TestTrue(TEXT("single ok"), Subsystem->QuerySync(
			Bank, TConstArrayView<float>(Queries.GetData() + M * 48, 48), Args, Single));
		TestEqual(TEXT("count"), BatchCounts[M], Single.Num());
		for (int32 i = 0; i < Single.Num(); ++i)
		{
			TestEqual(TEXT("index"), BatchHits[M * Args.K + i].Index, Single[i].Index);
			TestEqual(TEXT("score bits"), BatchHits[M * Args.K + i].Score, Single[i].Score);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSSerialParallelTest,
	"SuperFAISS.B.SerialParallelEquality",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSSerialParallelTest::RunTest(const FString& Parameters)
{
	// Plan §12 B1–B3 at the subsystem level: the serial and parallel scan paths return
	// bit-identical results (the top-k order is a strict total order, so chunk
	// scheduling cannot matter), the parallel path is repeat-deterministic, and ties
	// keep ascending-index order under parallel execution.
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	// 12000 x 96 int8 -> ~1.1 MB, ~18 chunks: enough for a real fan-out.
	USuperFAISSVectorBank* Bank = MakeBank(*this, 12000, 96);
	if (!Subsystem || !Bank)
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

	FSuperFAISSQueryArgs Args;
	Args.K = 20;

	constexpr int32 Variants = 6;
	TArray<TArray<FSuperFAISSHit>> Serial;
	TArray<TArray<FSuperFAISSHit>> Parallel;
	Serial.SetNum(Variants);
	Parallel.SetNum(Variants);

	ModeVar->Set(0); // force serial
	for (int32 V = 0; V < Variants; ++V)
	{
		TestTrue(TEXT("serial ok"),
			Subsystem->QuerySync(Bank, MakeQuery(96, 700 + V), Args, Serial[V]));
	}

	ModeVar->Set(2); // force parallel
	for (int32 V = 0; V < Variants; ++V)
	{
		TestTrue(TEXT("parallel ok"),
			Subsystem->QuerySync(Bank, MakeQuery(96, 700 + V), Args, Parallel[V]));

		// B1: parallel repeat is bit-identical to itself.
		TArray<FSuperFAISSHit> Repeat;
		TestTrue(TEXT("parallel repeat ok"),
			Subsystem->QuerySync(Bank, MakeQuery(96, 700 + V), Args, Repeat));
		TestEqual(TEXT("repeat count"), Repeat.Num(), Parallel[V].Num());
		for (int32 i = 0; i < FMath::Min(Repeat.Num(), Parallel[V].Num()); ++i)
		{
			TestEqual(TEXT("repeat index"), Repeat[i].Index, Parallel[V][i].Index);
			TestEqual(TEXT("repeat score"), Repeat[i].Score, Parallel[V][i].Score);
		}
	}

	// B2: serial == parallel, bit for bit.
	for (int32 V = 0; V < Variants; ++V)
	{
		TestEqual(TEXT("count"), Parallel[V].Num(), Serial[V].Num());
		for (int32 i = 0; i < FMath::Min(Parallel[V].Num(), Serial[V].Num()); ++i)
		{
			TestEqual(TEXT("index"), Parallel[V][i].Index, Serial[V][i].Index);
			TestEqual(TEXT("score bits"), Parallel[V][i].Score, Serial[V][i].Score);
		}
	}

	// B3: ties under parallel execution — rows 100..104 duplicated from row 7 must
	// come back in ascending index order.
	{
		uint64 State = 0xBADD00D5ull;
		TArray<float> Rows;
		const int32 Dims = 64;
		const int32 Count = 9000;
		Rows.SetNumUninitialized(Count * Dims);
		for (float& X : Rows)
		{
			State ^= State >> 12;
			State ^= State << 25;
			State ^= State >> 27;
			X = static_cast<float>(static_cast<int64>((State * 0x2545F4914F6CDD1Dull) >> 24)) /
				static_cast<float>(1ll << 39);
		}
		for (int32 R = 100; R < 105; ++R)
		{
			FMemory::Memcpy(&Rows[R * Dims], &Rows[7 * Dims], Dims * sizeof(float));
		}
		USuperFAISSVectorBank* TieBank = NewObject<USuperFAISSVectorBank>();
		FString Error;
		if (TestTrue(TEXT("tie bank"), TieBank->InitFromSource(Rows, Count, Dims,
			ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Float32, {}, TEXT(""), Error)))
		{
			TArray<float> Q(Rows.GetData() + 7 * Dims, Dims);
			FSuperFAISSQueryArgs TieArgs;
			TieArgs.K = 6;
			TArray<FSuperFAISSHit> Hits;
			TestTrue(TEXT("tie query ok"), Subsystem->QuerySync(TieBank, Q, TieArgs, Hits));
			const int32 Expected[6] = {7, 100, 101, 102, 103, 104};
			TestEqual(TEXT("tie hit count"), Hits.Num(), 6);
			for (int32 i = 0; i < FMath::Min(Hits.Num(), 6); ++i)
			{
				TestEqual(TEXT("tie order"), Hits[i].Index, Expected[i]);
			}
		}
	}

	ModeVar->Set(SavedMode);
	return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(
	FSuperFAISSSubsystemAsyncTest,
	"SuperFAISS.B.AsyncDeliveryAndCancel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

void FSuperFAISSSubsystemAsyncTest::GetTests(
	TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
	OutBeautifiedNames.Add(TEXT("Run"));
	OutTestCommands.Add(TEXT(""));
}

namespace
{
	struct FAsyncProbe
	{
		std::atomic<int32> Delivered{0};
		std::atomic<int32> Succeeded{0};
		std::atomic<int32> HitCount{-1};
	};

	// Latent command: waits until N deliveries arrive (or times out).
	DEFINE_LATENT_AUTOMATION_COMMAND_THREE_PARAMETER(FWaitForDeliveries,
		TSharedPtr<FAsyncProbe>, Probe, int32, Expected, double, Deadline);

	bool FWaitForDeliveries::Update()
	{
		return Probe->Delivered.load() >= Expected || FPlatformTime::Seconds() > Deadline;
	}

	DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FCheckAsyncResults,
		TSharedPtr<FAsyncProbe>, Probe, FAutomationTestBase*, Test);

	bool FCheckAsyncResults::Update()
	{
		// B4: both delegates delivered on the game thread; the normal query succeeded
		// with k hits. Cancellation is best-effort — a cancel that lands after the scan
		// legitimately reports success, so only the guaranteed properties are asserted.
		Test->TestEqual(TEXT("both delegates delivered"), Probe->Delivered.load(), 2);
		Test->TestTrue(TEXT("normal query succeeded"), Probe->Succeeded.load() >= 1);
		Test->TestEqual(TEXT("successful query returned k hits"), Probe->HitCount.load(), 5);
		return true;
	}
}

bool FSuperFAISSSubsystemAsyncTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	USuperFAISSVectorBank* Bank = MakeBank(*this, 3000, 32);
	if (!Subsystem || !Bank)
	{
		return true;
	}
	// B4's lifetime half: the async pin must keep the transient bank alive even if
	// GC runs while queries are in flight — force a collection during the wait.

	TSharedPtr<FAsyncProbe> Probe = MakeShared<FAsyncProbe>();

	FSuperFAISSQueryArgs Args;
	Args.K = 5;

	FSuperFAISSNativeResultDelegate OnNormal;
	OnNormal.BindLambda([Probe](const TArray<FSuperFAISSHit>& Hits, bool bSuccess)
	{
		Probe->Delivered.fetch_add(1);
		if (bSuccess)
		{
			Probe->Succeeded.fetch_add(1);
			Probe->HitCount.store(Hits.Num());
		}
	});
	Subsystem->QueryAsync(Bank, MakeQuery(32, 900), Args, OnNormal);

	FSuperFAISSNativeResultDelegate OnCancelled;
	OnCancelled.BindLambda([Probe](const TArray<FSuperFAISSHit>&, bool bSuccess)
	{
		Probe->Delivered.fetch_add(1);
		if (bSuccess)
		{
			Probe->Succeeded.fetch_add(1);
		}
	});
	FSuperFAISSTicket Ticket =
		Subsystem->QueryAsync(Bank, MakeQuery(32, 901), Args, OnCancelled);
	Ticket.Cancel();

	// Make the pin's job real: collect garbage while the tasks are in flight.
	GEngine->ForceGarbageCollection(true);

	ADD_LATENT_AUTOMATION_COMMAND(FWaitForDeliveries(Probe, 2, FPlatformTime::Seconds() + 10.0));
	ADD_LATENT_AUTOMATION_COMMAND(FCheckAsyncResults(Probe, this));
	return true;
}

namespace
{
	// P-3 (Poirot sweep): QueryAsync must run the SAME query the sync path would.
	// Pre-fix it copied only K/ExcludeBits/bScoreAsDot, so composed args
	// (channels, segments, bias, cross-device) silently ran plain and reported
	// success with wrong hits.
	struct FComposedAsyncProbe
	{
		std::atomic<int32> Delivered{0};
		std::atomic<bool> bSucceeded{false};
		TArray<FSuperFAISSHit> AsyncHits;
		TArray<FSuperFAISSHit> SyncHits;
	};

	DEFINE_LATENT_AUTOMATION_COMMAND_THREE_PARAMETER(FWaitComposedDelivery,
		TSharedPtr<FComposedAsyncProbe>, Probe, int32, Expected, double, Deadline);

	bool FWaitComposedDelivery::Update()
	{
		return Probe->Delivered.load() >= Expected || FPlatformTime::Seconds() > Deadline;
	}

	DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FCheckComposedAsync,
		TSharedPtr<FComposedAsyncProbe>, Probe, FAutomationTestBase*, Test);

	bool FCheckComposedAsync::Update()
	{
		Test->TestEqual(TEXT("composed async delivered"), Probe->Delivered.load(), 1);
		Test->TestTrue(TEXT("composed async succeeded"), Probe->bSucceeded.load());
		Test->TestEqual(TEXT("async hit count == sync"),
			Probe->AsyncHits.Num(), Probe->SyncHits.Num());
		for (int32 i = 0; i < Probe->SyncHits.Num() && i < Probe->AsyncHits.Num(); ++i)
		{
			Test->TestTrue(TEXT("async hit == sync hit"),
				Probe->AsyncHits[i].Index == Probe->SyncHits[i].Index &&
					Probe->AsyncHits[i].Score == Probe->SyncHits[i].Score);
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSComposedAsyncTest,
	"SuperFAISS.B.ComposedAsyncParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSComposedAsyncTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	USuperFAISSVectorBank* Bank = MakeBank(*this, 500, 32);
	if (!Subsystem || !Bank)
	{
		return true;
	}

	// A composed query: raw segments + a sparse bias pair (channels need a
	// schema-2 bank; segments exercise the same dropped-args path).
	FSuperFAISSQueryArgs Args;
	Args.K = 6;
	FSuperFAISSSegment SegA;
	SegA.Offset = 0;
	SegA.Length = Bank->PaddedDims / 2;
	SegA.Weight = 1.0f;
	FSuperFAISSSegment SegB;
	SegB.Offset = Bank->PaddedDims / 2;
	SegB.Length = Bank->PaddedDims - Bank->PaddedDims / 2;
	SegB.Weight = 0.25f;
	Args.Segments = {SegA, SegB};
	Args.BiasPairs = {{123, 5.0f}};

	const TArray<float> Query = MakeQuery(32, 777);
	TSharedPtr<FComposedAsyncProbe> Probe = MakeShared<FComposedAsyncProbe>();
	TestTrue(TEXT("sync composed"),
		Subsystem->QuerySync(Bank, Query, Args, Probe->SyncHits));

	FSuperFAISSNativeResultDelegate Done;
	Done.BindLambda([Probe](const TArray<FSuperFAISSHit>& Hits, bool bSuccess)
	{
		Probe->AsyncHits = Hits;
		Probe->bSucceeded.store(bSuccess);
		Probe->Delivered.fetch_add(1);
	});
	Subsystem->QueryAsync(Bank, Query, Args, MoveTemp(Done));

	ADD_LATENT_AUTOMATION_COMMAND(FWaitComposedDelivery(Probe, 1, FPlatformTime::Seconds() + 10.0));
	ADD_LATENT_AUTOMATION_COMMAND(FCheckComposedAsync(Probe, this));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
