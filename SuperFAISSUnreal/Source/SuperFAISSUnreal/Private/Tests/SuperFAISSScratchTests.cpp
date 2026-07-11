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

	// R-5 (Poirot deep review): freezing zero live rows yields an EMPTY VALID
	// bank, not a null indistinguishable from failure.
	{
		USuperFAISSScratchBank* Fresh = NewObject<USuperFAISSScratchBank>();
		TestTrue(TEXT("r5 init"), Fresh->Init(8, Dims,
			ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Float32));
		TArray<int32> EmptyMap;
		USuperFAISSVectorBank* FrozenEmpty = Fresh->Freeze(EmptyMap);
		if (TestNotNull(TEXT("empty freeze returns a bank"), FrozenEmpty))
		{
			TestTrue(TEXT("empty frozen valid"), FrozenEmpty->IsValid());
			TestEqual(TEXT("empty frozen count"), FrozenEmpty->Count, 0);
		}
		TestEqual(TEXT("empty map"), EmptyMap.Num(), 0);

		// All-removed: same outcome, map all -1.
		TArray<float> Row = ScratchRows(1, Dims, 0x0F0Full);
		int32 Index = INDEX_NONE;
		TestTrue(TEXT("r5 append"), Fresh->Append(Row, Index));
		TestTrue(TEXT("r5 remove"), Fresh->Remove(Index));
		TArray<int32> Map;
		USuperFAISSVectorBank* FrozenRemoved = Fresh->Freeze(Map);
		if (TestNotNull(TEXT("all-removed freeze returns a bank"), FrozenRemoved))
		{
			TestTrue(TEXT("all-removed frozen valid"), FrozenRemoved->IsValid());
			TestEqual(TEXT("all-removed frozen count"), FrozenRemoved->Count, 0);
		}
		TestEqual(TEXT("all-removed map"), Map.Num(), 1);
		if (Map.Num() == 1)
		{
			TestEqual(TEXT("all-removed map entry"), Map[0], -1);
		}
	}

	// P-2 (Poirot sweep): querying an EMPTY scratch bank is well-defined - true
	// with zero hits - regardless of the pooled workspace's history (pre-fix it
	// returned false on a cold buffer and true on a warm one).
	{
		USuperFAISSScratchBank* Empty = NewObject<USuperFAISSScratchBank>();
		TestTrue(TEXT("empty init"), Empty->Init(16, Dims,
			ESuperFAISSBankMetric::Dot, ESuperFAISSBankQuantization::Float32));
		TArray<float> Query = ScratchRows(1, Dims, 0xE117ull);
		FSuperFAISSQueryArgs Args;
		Args.K = 4;
		TArray<FSuperFAISSHit> Hits;
		TestTrue(TEXT("empty-bank query succeeds"),
			GEngine->GetEngineSubsystem<USuperFAISSSubsystem>()->QueryScratch(
				Empty, Query, Args, Hits));
		TestEqual(TEXT("empty-bank zero hits"), Hits.Num(), 0);
	}

	// P-1 (Poirot sweep): appends racing pinned queries - the scratch bank's
	// designed model - must never corrupt staging. The fix sizes tombstone
	// staging from capacity (pin-stable); this storm drives appends across many
	// 32-row word boundaries while queries run, and every returned hit must be a
	// valid, live row index. (The pre-fix overrun needs an exact interleaving a
	// test cannot force deterministically; the capacity invariant it violates is
	// what this pins, alongside the sweep's written argument.)
	{
		USuperFAISSSubsystem* Sub = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
		USuperFAISSScratchBank* Storm = NewObject<USuperFAISSScratchBank>();
		const int32 StormCap = 512;
		TestTrue(TEXT("storm init"), Storm->Init(StormCap, Dims,
			ESuperFAISSBankMetric::Dot, ESuperFAISSBankQuantization::Float32));
		const TArray<float> StormRows = ScratchRows(StormCap, Dims, 0x570A11ull);
		TArray<float> Seed;
		Seed.SetNumUninitialized(Dims);
		int32 Index = INDEX_NONE;
		FMemory::Memcpy(Seed.GetData(), StormRows.GetData(), Dims * sizeof(float));
		TestTrue(TEXT("storm seed"), Storm->Append(Seed, Index));

		std::atomic<bool> Done{false};
		std::atomic<int32> Violations{0};
		auto Future = Async(EAsyncExecution::Thread, [&]()
		{
			// Reader thread: pinned queries while the writer below appends.
			TArray<float> Query = ScratchRows(1, Dims, 0x9E11ull);
			FSuperFAISSQueryArgs Args;
			Args.K = 8;
			TArray<FSuperFAISSHit> Hits;
			while (!Done.load())
			{
				if (!Sub->QueryScratch(Storm, Query, Args, Hits))
				{
					continue; // draining windows are legal refusals
				}
				const int32 Count = Storm->GetCount();
				for (const FSuperFAISSHit& Hit : Hits)
				{
					if (Hit.Index < 0 || Hit.Index >= Count)
					{
						Violations.fetch_add(1);
					}
				}
			}
		});
		TArray<float> Row;
		Row.SetNumUninitialized(Dims);
		for (int32 R = 1; R < StormCap; ++R)
		{
			FMemory::Memcpy(Row.GetData(), StormRows.GetData() + R * Dims,
				Dims * sizeof(float));
			TestTrue(TEXT("storm append"), Storm->Append(Row, Index));
		}
		Done.store(true);
		Future.Wait();
		TestEqual(TEXT("storm violations"), Violations.load(), 0);
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

// T-V2.3-U1 — scratch recall audit at the plugin surface (V2 plan section 20):
// the plugin MeasureRecall entry point returns the SAME number the core routine
// returns on the same rows and seed (the plugin wraps the core type — one math);
// a non-retention bank surfaces the InvalidArgument-mapped failure; a later
// mutation renders the report stale, never silently current; FreezeWithRecall
// carries a fresh number measured over the compacted rows.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSScratchRecallTest,
	"SuperFAISS.A.ScratchRecall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSScratchRecallTest::RunTest(const FString& Parameters)
{
	constexpr int32 Count = 160;
	constexpr int32 Dims = 32;
	const TArray<float> Rows = ScratchRows(Count, Dims, 0x2ECA11ull);

	// Retention is opt-in at Init, never the default, and descriptor-visible.
	USuperFAISSScratchBank* Plain = NewObject<USuperFAISSScratchBank>();
	TestTrue(TEXT("default init"), Plain->Init(Count, Dims,
		ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Int8));
	TestFalse(TEXT("retention never the default"), Plain->RetainsFloats());

	USuperFAISSScratchBank* Audited = NewObject<USuperFAISSScratchBank>();
	TestTrue(TEXT("retention init"), Audited->Init(Count, Dims,
		ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Int8,
		/*bRetainFloats*/ true));
	TestTrue(TEXT("retention flag reads back"), Audited->RetainsFloats());

	// Identical history on the plugin bank and a direct core bank.
	superfaiss::ScratchBank CoreTwin;
	TestTrue(TEXT("core twin created"),
		CoreTwin.Create(Count, Dims, superfaiss::Metric::Cosine,
			superfaiss::Quantization::Int8, /*retainFloats*/ true) ==
			superfaiss::Status::Ok);
	TArray<float> Row;
	Row.SetNumUninitialized(Dims);
	for (int32 R = 0; R < Count; ++R)
	{
		FMemory::Memcpy(Row.GetData(), Rows.GetData() + R * Dims, Dims * sizeof(float));
		int32 Index = INDEX_NONE;
		TestTrue(TEXT("plugin append"), Audited->Append(Row, Index));
		TestTrue(TEXT("plain append"), Plain->Append(Row, Index));
		TestTrue(TEXT("core append"),
			CoreTwin.Append(Row.GetData(), Dims, nullptr) == superfaiss::Status::Ok);
	}
	for (int32 R = 0; R < Count; R += 11)
	{
		TestTrue(TEXT("plugin remove"), Audited->Remove(R));
		TestTrue(TEXT("core remove"), CoreTwin.Remove(R) == superfaiss::Status::Ok);
	}

	// U1 equality: plugin number bit-equals core number on the same rows and seed
	// (the plugin uses the core default seed; so does the direct call).
	FSuperFAISSScratchRecallReport Report;
	TestTrue(TEXT("plugin measure"), Audited->MeasureRecall(Report));
	superfaiss::Workspace CoreWs;
	superfaiss::ScratchRecallReport CoreReport;
	TestTrue(TEXT("core measure"),
		CoreTwin.MeasureScratchRecall(CoreWs, &CoreReport) == superfaiss::Status::Ok);
	TestTrue(TEXT("plugin recall bit-equals core"),
		Report.Recall == CoreReport.recall);
	TestEqual(TEXT("k"), Report.K, CoreReport.k);
	TestEqual(TEXT("sample count"), Report.SampleCount, CoreReport.sampleCount);
	TestEqual(TEXT("live rows"), Report.LiveRows, CoreReport.liveRows);
	TestEqual(TEXT("seed"), Report.Seed, static_cast<int64>(CoreReport.seed));
	TestEqual(TEXT("informative"), Report.bInformative, CoreReport.informative);

	// The report is cached for the descriptor surface, current at measurement.
	FSuperFAISSScratchRecallReport Cached;
	bool bStale = true;
	TestTrue(TEXT("cached report"), Audited->GetLastRecallReport(Cached, bStale));
	TestTrue(TEXT("cached equals measured"), Cached.Recall == Report.Recall);
	TestFalse(TEXT("current at measurement"), bStale);
	TestFalse(TEXT("stale check agrees"), Audited->IsRecallReportStale(Report));

	// A later mutation renders it stale — read as stale, never silently current.
	{
		FMemory::Memcpy(Row.GetData(), Rows.GetData(), Dims * sizeof(float));
		int32 Index = INDEX_NONE;
		TestFalse(TEXT("full bank rejects append"), Audited->Append(Row, Index));
		TestTrue(TEXT("mutating remove"), Audited->Remove(1));
		TestTrue(TEXT("report reads stale"), Audited->IsRecallReportStale(Report));
		TestTrue(TEXT("cache reads stale"), Audited->GetLastRecallReport(Cached, bStale));
		TestTrue(TEXT("cache stale flag"), bStale);
	}

	// Reject-over-degrade: a non-retention bank is a defined failure, not a guess.
	FSuperFAISSScratchRecallReport Rejected;
	TestFalse(TEXT("non-retention measure rejected"), Plain->MeasureRecall(Rejected));

	// FreezeWithRecall: the graduated bank carries a FRESH number measured over the
	// compacted rows — INV-equal to a direct measurement taken at freeze time.
	{
		FSuperFAISSScratchRecallReport Direct;
		TestTrue(TEXT("direct pre-freeze measure"), Audited->MeasureRecall(Direct));
		TArray<int32> IndexMap;
		FSuperFAISSScratchRecallReport Frozen;
		bool bMeasured = false;
		USuperFAISSVectorBank* Graduated =
			Audited->FreezeWithRecall(IndexMap, Frozen, bMeasured);
		TestNotNull(TEXT("frozen bank"), Graduated);
		TestTrue(TEXT("freeze measured"), bMeasured);
		TestTrue(TEXT("freeze recall == direct measure"), Frozen.Recall == Direct.Recall);

		// A non-retention freeze produces no number.
		TArray<int32> PlainMap;
		FSuperFAISSScratchRecallReport None;
		bool bPlainMeasured = true;
		USuperFAISSVectorBank* PlainFrozen =
			Plain->FreezeWithRecall(PlainMap, None, bPlainMeasured);
		TestNotNull(TEXT("plain frozen bank"), PlainFrozen);
		TestFalse(TEXT("no number on a non-retention freeze"), bPlainMeasured);
	}

	// Retention serializes: the loaded bank keeps its audit capability and returns
	// the same number on the same seed (bit-equal round trip).
	{
		USuperFAISSScratchBank* Reloaded = NewObject<USuperFAISSScratchBank>();
		TArray<uint8> Bytes;
		TestTrue(TEXT("save"), Audited->SaveToBytes(Bytes));
		TestTrue(TEXT("load"), Reloaded->LoadFromBytes(Bytes));
		TestTrue(TEXT("retention survives the round trip"), Reloaded->RetainsFloats());
		FSuperFAISSScratchRecallReport A;
		FSuperFAISSScratchRecallReport B;
		TestTrue(TEXT("original measures"), Audited->MeasureRecall(A));
		TestTrue(TEXT("reloaded measures"), Reloaded->MeasureRecall(B));
		TestTrue(TEXT("round-trip recall bit-equal"), A.Recall == B.Recall);
	}

	return true;
}

// Review S1 / Japp S-1 — staleness across Save/Load at the plugin surface: a
// recall report taken before a LoadFromBytes must read STALE after it, never
// silently current. The trap is the append-only same-count restore (a save-game
// round trip): the pre-fix core reset the generation to the loaded row count —
// exactly an append-only bank's stamp — and the plugin kept its cached report,
// so a number measured on rows that no longer exist read as current.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSScratchRecallLoadStalenessTest,
	"SuperFAISS.A.ScratchRecallLoadStaleness",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSScratchRecallLoadStalenessTest::RunTest(const FString& Parameters)
{
	constexpr int32 Count = 48;
	constexpr int32 Dims = 24;
	const TArray<float> Rows = ScratchRows(Count, Dims, 0x10AD5ull);

	// Append-only retention bank: no removes, so its generation equals its count —
	// the collision case.
	USuperFAISSScratchBank* Bank = NewObject<USuperFAISSScratchBank>();
	TestTrue(TEXT("init"), Bank->Init(Count, Dims, ESuperFAISSBankMetric::Dot,
		ESuperFAISSBankQuantization::Int8, /*bRetainFloats*/ true));
	TArray<float> Row;
	Row.SetNumUninitialized(Dims);
	for (int32 R = 0; R < Count; ++R)
	{
		FMemory::Memcpy(Row.GetData(), Rows.GetData() + R * Dims, Dims * sizeof(float));
		int32 Index = INDEX_NONE;
		TestTrue(TEXT("append"), Bank->Append(Row, Index));
	}

	FSuperFAISSScratchRecallReport Before;
	TestTrue(TEXT("measured"), Bank->MeasureRecall(Before));
	TestFalse(TEXT("current at measurement"), Bank->IsRecallReportStale(Before));

	// The same-count restore: save, load the same blob back into the same object.
	TArray<uint8> Blob;
	TestTrue(TEXT("save"), Bank->SaveToBytes(Blob));
	TestTrue(TEXT("load"), Bank->LoadFromBytes(Blob));

	// The caller-held report reads stale, and the bank's cached report never reads
	// current — cleared or stale, either is lawful; current is the defect.
	TestTrue(TEXT("pre-load report reads stale after Load"),
		Bank->IsRecallReportStale(Before));
	FSuperFAISSScratchRecallReport Cached;
	bool bStale = false;
	const bool bHasCached = Bank->GetLastRecallReport(Cached, bStale);
	TestTrue(TEXT("no cached report reads current after Load"), !bHasCached || bStale);

	// The mutated-restore case: report, save, mutate, restore the pre-mutation
	// blob — the report must read stale against the restored rows too.
	FSuperFAISSScratchRecallReport Mid;
	TestTrue(TEXT("re-measured"), Bank->MeasureRecall(Mid));
	TestFalse(TEXT("fresh report current"), Bank->IsRecallReportStale(Mid));
	TArray<uint8> Blob2;
	TestTrue(TEXT("save 2"), Bank->SaveToBytes(Blob2));
	TestTrue(TEXT("mutating remove"), Bank->Remove(1));
	TestTrue(TEXT("stale by the remove"), Bank->IsRecallReportStale(Mid));
	TestTrue(TEXT("restore"), Bank->LoadFromBytes(Blob2));
	TestTrue(TEXT("pre-save report stale after the restore"),
		Bank->IsRecallReportStale(Mid));

	// A report taken after the Load is current — the staleness is the boundary,
	// not a permanently-poisoned bank.
	FSuperFAISSScratchRecallReport Fresh;
	TestTrue(TEXT("post-load measure"), Bank->MeasureRecall(Fresh));
	TestFalse(TEXT("post-load report current"), Bank->IsRecallReportStale(Fresh));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
