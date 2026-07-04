// Scratch-bank tests (V2 plan slot 4, T-V2-F): the UE surface over the core
// scratch bank — append/remove/query through the subsystem's pinned dispatch,
// freeze to a bit-identical immutable bank, byte round-trip, the N4 drain gate,
// and the rejection matrix. The core's own storm/serialize proofs are T22.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/Async.h"
#include "SuperFAISSScratchBank.h"
#include "SuperFAISSSubsystem.h"
#include "SuperFAISSVectorBank.h"

namespace
{
	TArray<float> ScratchRows(int32 Count, int32 Dims, uint64 Seed)
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSScratchBankTest,
	"SuperFAISS.A.ScratchBank",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSScratchBankTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	constexpr int32 Count = 200;
	constexpr int32 Dims = 32;

	for (ESuperFAISSBankQuantization Quant :
		{ESuperFAISSBankQuantization::Float32, ESuperFAISSBankQuantization::Int8})
	{
		const TArray<float> Rows = ScratchRows(Count, Dims, 0x5C247Cull);
		const TArray<float> Query = ScratchRows(1, Dims, 0x9E11ull);

		USuperFAISSScratchBank* Scratch = NewObject<USuperFAISSScratchBank>();
		TestTrue(TEXT("init"), Scratch->Init(Count, Dims,
			ESuperFAISSBankMetric::Cosine, Quant));
		TestFalse(TEXT("double init rejected"), Scratch->Init(Count, Dims,
			ESuperFAISSBankMetric::Cosine, Quant));

		TArray<float> Row;
		Row.SetNumUninitialized(Dims);
		for (int32 R = 0; R < Count; ++R)
		{
			FMemory::Memcpy(Row.GetData(), Rows.GetData() + R * Dims, Dims * sizeof(float));
			int32 Index = INDEX_NONE;
			TestTrue(TEXT("append"), Scratch->Append(Row, Index));
			TestEqual(TEXT("append index"), Index, R);
		}
		TestEqual(TEXT("count"), Scratch->GetCount(), Count);

		// Scratch query == the equivalent imported bank's query, bit-identical
		// (append normalizes/quantizes with the importer's math).
		USuperFAISSVectorBank* Imported = NewObject<USuperFAISSVectorBank>();
		FString Error;
		TestTrue(TEXT("imported twin"), Imported->InitFromSource(Rows, Count, Dims,
			ESuperFAISSBankMetric::Cosine, Quant, {}, TEXT("twin"), Error));

		FSuperFAISSQueryArgs Args;
		Args.K = 10;
		TArray<FSuperFAISSHit> ScratchHits, BankHits;
		TestTrue(TEXT("scratch query"),
			Subsystem->QueryScratch(Scratch, Query, Args, ScratchHits));
		TestTrue(TEXT("bank query"), Subsystem->QuerySync(Imported, Query, Args, BankHits));
		TestEqual(TEXT("hit count"), ScratchHits.Num(), BankHits.Num());
		for (int32 i = 0; i < FMath::Min(ScratchHits.Num(), BankHits.Num()); ++i)
		{
			TestTrue(TEXT("scratch == imported hit"),
				ScratchHits[i].Index == BankHits[i].Index &&
					ScratchHits[i].Score == BankHits[i].Score);
		}

		// Removal is exclusion on the next query.
		const int32 Victim = ScratchHits[0].Index;
		TestTrue(TEXT("remove"), Scratch->Remove(Victim));
		TestEqual(TEXT("live count"), Scratch->GetLiveCount(), Count - 1);
		TArray<FSuperFAISSHit> AfterRemove;
		TestTrue(TEXT("post-remove query"),
			Subsystem->QueryScratch(Scratch, Query, Args, AfterRemove));
		for (const FSuperFAISSHit& Hit : AfterRemove)
		{
			TestTrue(TEXT("victim excluded"), Hit.Index != Victim);
		}

		// Freeze: compacted immutable bank, scores bit-identical through the map.
		TArray<int32> IndexMap;
		USuperFAISSVectorBank* Frozen = Scratch->Freeze(IndexMap);
		if (TestNotNull(TEXT("frozen bank"), Frozen))
		{
			TestTrue(TEXT("frozen valid"), Frozen->IsValid());
			TestEqual(TEXT("frozen count"), Frozen->Count, Count - 1);
			TestEqual(TEXT("map size"), IndexMap.Num(), Count);
			TestEqual(TEXT("victim dropped"), IndexMap[Victim], -1);
			TArray<FSuperFAISSHit> FrozenHits;
			TestTrue(TEXT("frozen query"),
				Subsystem->QuerySync(Frozen, Query, Args, FrozenHits));
			TestEqual(TEXT("frozen hit count"), FrozenHits.Num(), AfterRemove.Num());
			for (int32 i = 0; i < FMath::Min(FrozenHits.Num(), AfterRemove.Num()); ++i)
			{
				TestTrue(TEXT("frozen == scratch through map"),
					FrozenHits[i].Index == IndexMap[AfterRemove[i].Index] &&
						FrozenHits[i].Score == AfterRemove[i].Score);
			}
		}

		// Byte round-trip: identical state, identical answers.
		TArray<uint8> Bytes;
		TestTrue(TEXT("save"), Scratch->SaveToBytes(Bytes));
		USuperFAISSScratchBank* Loaded = NewObject<USuperFAISSScratchBank>();
		TestTrue(TEXT("load"), Loaded->LoadFromBytes(Bytes));
		TestEqual(TEXT("loaded count"), Loaded->GetCount(), Scratch->GetCount());
		TestEqual(TEXT("loaded live"), Loaded->GetLiveCount(), Scratch->GetLiveCount());
		TArray<FSuperFAISSHit> LoadedHits;
		TestTrue(TEXT("loaded query"),
			Subsystem->QueryScratch(Loaded, Query, Args, LoadedHits));
		TestEqual(TEXT("loaded hit count"), LoadedHits.Num(), AfterRemove.Num());
		for (int32 i = 0; i < FMath::Min(LoadedHits.Num(), AfterRemove.Num()); ++i)
		{
			TestTrue(TEXT("loaded == pre-save hit"),
				LoadedHits[i].Index == AfterRemove[i].Index &&
					LoadedHits[i].Score == AfterRemove[i].Score);
		}
		// A corrupt blob is rejected and leaves the loaded state unchanged.
		TArray<uint8> Bad = Bytes;
		Bad[0] ^= 0xFF;
		TestFalse(TEXT("corrupt blob rejected"), Loaded->LoadFromBytes(Bad));
		TestEqual(TEXT("state unchanged"), Loaded->GetCount(), Scratch->GetCount());
	}

	// Grow preserves indices and opens capacity; the capacity refusal precedes it.
	{
		USuperFAISSScratchBank* Bank = NewObject<USuperFAISSScratchBank>();
		TestTrue(TEXT("small init"), Bank->Init(4, Dims,
			ESuperFAISSBankMetric::Dot, ESuperFAISSBankQuantization::Float32));
		const TArray<float> Rows = ScratchRows(5, Dims, 0x60011ull);
		TArray<float> Row;
		Row.SetNumUninitialized(Dims);
		int32 Index = INDEX_NONE;
		for (int32 R = 0; R < 4; ++R)
		{
			FMemory::Memcpy(Row.GetData(), Rows.GetData() + R * Dims, Dims * sizeof(float));
			TestTrue(TEXT("fill"), Bank->Append(Row, Index));
		}
		FMemory::Memcpy(Row.GetData(), Rows.GetData() + 4 * Dims, Dims * sizeof(float));
		TestFalse(TEXT("capacity refusal"), Bank->Append(Row, Index));
		TestTrue(TEXT("grow"), Bank->Grow(8));
		TestEqual(TEXT("capacity"), Bank->GetCapacity(), 8);
		TestEqual(TEXT("count preserved"), Bank->GetCount(), 4);
		TestTrue(TEXT("post-grow append"), Bank->Append(Row, Index));
		TestEqual(TEXT("post-grow index"), Index, 4);
	}

	// The N4 drain gate: while a pin is held, Grow waits and QueryScratch (via
	// TryPin) is refused; releasing the pin lets the Grow complete.
	{
		USuperFAISSSubsystem* Sub = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
		USuperFAISSScratchBank* Bank = NewObject<USuperFAISSScratchBank>();
		TestTrue(TEXT("gate init"), Bank->Init(4, Dims,
			ESuperFAISSBankMetric::Dot, ESuperFAISSBankQuantization::Float32));
		TArray<float> Row = ScratchRows(1, Dims, 0x1234ull);
		int32 Index = INDEX_NONE;
		TestTrue(TEXT("gate append"), Bank->Append(Row, Index));

		TestTrue(TEXT("pin"), Bank->TryPin());
		std::atomic<bool> GrowDone{false};
		std::atomic<bool> GrowResult{false};
		auto Future = Async(EAsyncExecution::Thread, [Bank, &GrowDone, &GrowResult]() {
			GrowResult.store(Bank->Grow(16));
			GrowDone.store(true);
		});
		FPlatformProcess::Sleep(0.05f);
		TestFalse(TEXT("grow blocked by pin"), GrowDone.load());
		// The waiting drain refuses new queries — the dispatch-point gate.
		TArray<FSuperFAISSHit> Hits;
		FSuperFAISSQueryArgs Args;
		Args.K = 1;
		TestFalse(TEXT("query refused during drain"),
			Sub->QueryScratch(Bank, Row, Args, Hits));
		Bank->Unpin();
		Future.Wait();
		TestTrue(TEXT("grow completed"), GrowDone.load() && GrowResult.load());
		TestEqual(TEXT("grown capacity"), Bank->GetCapacity(), 16);
		TestTrue(TEXT("query works after drain"),
			Sub->QueryScratch(Bank, Row, Args, Hits));
	}

	// Rejections: dims mismatch, zero-norm Cosine append, channels on scratch.
	{
		USuperFAISSScratchBank* Bank = NewObject<USuperFAISSScratchBank>();
		TestTrue(TEXT("cosine init"), Bank->Init(4, Dims,
			ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Float32));
		TArray<float> Short;
		Short.SetNumZeroed(Dims / 2);
		int32 Index = INDEX_NONE;
		TestFalse(TEXT("dims mismatch rejected"), Bank->Append(Short, Index));
		TArray<float> Zero;
		Zero.SetNumZeroed(Dims);
		TestFalse(TEXT("zero-norm rejected"), Bank->Append(Zero, Index));

		TArray<float> Good = ScratchRows(1, Dims, 0x77ull);
		TestTrue(TEXT("good append"), Bank->Append(Good, Index));
		FSuperFAISSQueryArgs Args;
		Args.K = 1;
		Args.Channels = {{TEXT("identity"), 1.0f}};
		TArray<FSuperFAISSHit> Hits;
		TestFalse(TEXT("channels rejected on scratch"),
			GEngine->GetEngineSubsystem<USuperFAISSSubsystem>()->QueryScratch(
				Bank, Good, Args, Hits));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
