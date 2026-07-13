// V3.0 slot-5 red suite (plan §23.4/§23.5/§23.9 slot 5): the plugin channel surface
// over a live scratch bank — channel-capable InitWithChannels + construction-time
// validation, per-channel scratch Query (the QueryScratch guard dropped, V3-G3),
// per-channel scratch Decompose, channel-aware Freeze to a schemaVersion-2 asset,
// the plugin scratch channel-name resolver (V3-G8/N1), the per-channel recall
// reporting surface (D-V3-7), and the channel-scoped analytics BP surface (§23.5).
//
// Authored red-first, before the implementation (Curie): every cell drives a plugin
// method Hastings has scaffolded to a trivial failure. The feature oracle for the
// scratch channel query/decompose/freeze is the equivalent baked channel bank (built
// through the shipped, FEAT-proven InitFromSource channel path) — the plugin claim is
// that the scratch surface reaches the core per-channel scoring over the snapshot
// exactly as the baked path does. Slot 5 is the two-step gate (§23.9): the core
// channel surface over BP here, the tooling/UX (MCP) surface in SuperFAISSMCPTests.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "SuperFAISSScratchBank.h"
#include "SuperFAISSSubsystem.h"
#include "SuperFAISSVectorBank.h"

namespace
{
	// The shared xorshift row generator (bit-identical to the other suites' fixtures).
	TArray<float> ChanScratchRows(int32 Count, int32 Dims, uint64 Seed)
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

	// A two-channel table on the 16-grid: identity = first half, appearance = second.
	const TArray<FName>& ChanNames()
	{
		static const TArray<FName> Names = {TEXT("identity"), TEXT("appearance")};
		return Names;
	}

	// Appends Count rows of `Rows` to `Bank` (Dims-strided), asserting each append.
	void AppendRows(FAutomationTestBase& Test, USuperFAISSScratchBank* Bank,
		const TArray<float>& Rows, int32 Count, int32 Dims)
	{
		TArray<float> Row;
		Row.SetNumUninitialized(Dims);
		for (int32 R = 0; R < Count; ++R)
		{
			FMemory::Memcpy(Row.GetData(), Rows.GetData() + R * Dims, Dims * sizeof(float));
			int32 Index = INDEX_NONE;
			// Under the RED scaffold the bank is never created, so appends fail — that
			// cascade is the intended red state, not a test bug.
			Bank->Append(Row, Index);
		}
	}
}

// T-V3-U1 (cell 1, step A): channel-capable InitWithChannels + construction-time
// validation. A valid channel table creates a bank that honors its channel count and
// capacity; a malformed table (overlap, off-grid, out-of-bounds, > kMaxChannels,
// duplicate names, array-length mismatch) is rejected. The rejection cells are folded
// into a discrimination assertion (valid accepted WHILE the malformed one is refused)
// so a stub that fails every init cannot mark them swept.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSChannelScratchCreateTest,
	"SuperFAISS.A.ChannelScratchCreate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSChannelScratchCreateTest::RunTest(const FString& Parameters)
{
	constexpr int32 Cap = 64;
	constexpr int32 Dims = 32; // multiple of the int8 element grid (16)

	for (ESuperFAISSBankQuantization Quant :
		{ESuperFAISSBankQuantization::Float32, ESuperFAISSBankQuantization::Int8})
	{
		// A valid two-channel table: {identity:[0,16), appearance:[16,16)}.
		USuperFAISSScratchBank* Bank = NewObject<USuperFAISSScratchBank>();
		const bool bValidOk = Bank->InitWithChannels(Cap, Dims,
			ESuperFAISSBankMetric::Cosine, Quant, ChanNames(), {0, 16}, {16, 16},
			/*bRetainFloats*/ false);
		TestTrue(TEXT("valid channel table accepted"), bValidOk);
		TestTrue(TEXT("initialized after valid init"), Bank->IsInitialized());
		TestEqual(TEXT("channel count"), Bank->GetChannelCount(), 2);
		TestEqual(TEXT("identity index"), Bank->GetChannelIndex(TEXT("identity")), 0);
		TestEqual(TEXT("appearance index"), Bank->GetChannelIndex(TEXT("appearance")), 1);
		TestEqual(TEXT("unknown name -> INDEX_NONE"),
			Bank->GetChannelIndex(TEXT("nope")), INDEX_NONE);
		TestFalse(TEXT("double init rejected"), Bank->InitWithChannels(Cap, Dims,
			ESuperFAISSBankMetric::Cosine, Quant, ChanNames(), {0, 16}, {16, 16}, false));

		// Rejection catalog: each malformed table on a FRESH bank, folded into a
		// discrimination assert against the valid outcome.
		auto Rejects = [&](const TCHAR* Label, const TArray<FName>& N,
			const TArray<int32>& Off, const TArray<int32>& Len)
		{
			USuperFAISSScratchBank* B = NewObject<USuperFAISSScratchBank>();
			const bool bOk = B->InitWithChannels(Cap, Dims, ESuperFAISSBankMetric::Cosine,
				Quant, N, Off, Len, false);
			TestTrue(Label, bValidOk && !bOk);
		};
		Rejects(TEXT("overlap rejected (valid accepted)"),
			{TEXT("a"), TEXT("b")}, {0, 8}, {16, 16}); // [0,16) and [8,24) overlap
		Rejects(TEXT("out-of-bounds length rejected (valid accepted)"),
			{TEXT("a")}, {0}, {64}); // 64 > Dims
		Rejects(TEXT("array-length mismatch rejected (valid accepted)"),
			{TEXT("a"), TEXT("b")}, {0}, {16, 16}); // names/offsets mismatch
		Rejects(TEXT("duplicate names rejected (valid accepted)"),
			{TEXT("a"), TEXT("a")}, {0, 16}, {16, 16});
		// > kMaxChannels (8): a structurally-valid 9-channel table on a wider grid,
		// fired solely by the count.
		{
			TArray<FName> N9;
			TArray<int32> Off9;
			TArray<int32> Len9;
			for (int32 C = 0; C < 9; ++C)
			{
				N9.Add(FName(*FString::Printf(TEXT("c%d"), C)));
				Off9.Add(C * 16);
				Len9.Add(16);
			}
			USuperFAISSScratchBank* B = NewObject<USuperFAISSScratchBank>();
			const bool bOk = B->InitWithChannels(9 * 16, 9 * 16,
				ESuperFAISSBankMetric::Cosine, Quant, N9, Off9, Len9, false);
			TestTrue(TEXT("> kMaxChannels rejected (valid accepted)"), bValidOk && !bOk);
		}
	}

	// Off-grid is int8-specific (the float32 grid is 4): offset 4 is off the 16-grid.
	{
		USuperFAISSScratchBank* B = NewObject<USuperFAISSScratchBank>();
		const bool bOk = B->InitWithChannels(Cap, Dims, ESuperFAISSBankMetric::Cosine,
			ESuperFAISSBankQuantization::Int8, {TEXT("a")}, {4}, {16}, false);
		TestFalse(TEXT("off-grid int8 offset rejected"), bOk);
		TestFalse(TEXT("off-grid bank not initialized"), B->IsInitialized());
	}

	return true;
}

// T-V3-U2 (cells 2 + 5, step A): per-channel scratch Query, the QueryScratch guard
// drop (V3-G3), and the plugin scratch channel-name resolver (V3-G8/N1). A
// named-channel query on a channel-carrying scratch snapshot returns the same top-k
// as the equivalent baked channel bank (the feature oracle), a raw whole-vector query
// is unchanged, and the resolver rejects an unknown name and a channel-less bank —
// its own resolution, NOT the baked USuperFAISSSubsystem::ResolveSegments (typed on
// USuperFAISSVectorBank; see §11 routed finding N1).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSChannelScratchQueryTest,
	"SuperFAISS.A.ChannelScratchQuery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSChannelScratchQueryTest::RunTest(const FString& Parameters)
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
		const TArray<float> Rows = ChanScratchRows(Count, Dims, 0xC4A5C2ull);
		const TArray<float> Query = ChanScratchRows(1, Dims, 0x9E11ull);

		USuperFAISSScratchBank* Scratch = NewObject<USuperFAISSScratchBank>();
		Scratch->InitWithChannels(Count, Dims, ESuperFAISSBankMetric::Cosine, Quant,
			ChanNames(), {0, 16}, {16, 16}, false);
		AppendRows(*this, Scratch, Rows, Count, Dims);

		// The baked twin: same rows, same channel table, through the shipped bake.
		USuperFAISSVectorBank* Twin = NewObject<USuperFAISSVectorBank>();
		FString Error;
		TestTrue(TEXT("baked twin built"), Twin->InitFromSource(Rows, Count, Dims,
			ESuperFAISSBankMetric::Cosine, Quant, {}, TEXT("chan-twin"), Error,
			ChanNames(), {0, 16}, {16, 16}));

		FSuperFAISSQueryArgs Named;
		Named.K = 10;
		Named.Channels = {{TEXT("identity"), 1.0f}, {TEXT("appearance"), 0.25f}};

		// The guard-drop assertion the §23.9 slot-5 gate names explicitly: a passing
		// named-channel scratch query.
		TArray<FSuperFAISSHit> ScratchHits;
		const bool bScratchOk = Subsystem->QueryScratch(Scratch, Query, Named, ScratchHits);
		TestTrue(TEXT("named-channel scratch query succeeds (guard dropped, V3-G3)"),
			bScratchOk);

		TArray<FSuperFAISSHit> TwinHits;
		const bool bTwinOk = Subsystem->QuerySync(Twin, Query, Named, TwinHits);
		TestTrue(TEXT("baked twin channel query"), bTwinOk);

		// Feature oracle: scratch per-channel top-k == baked twin per-channel top-k.
		if (bScratchOk && bTwinOk)
		{
			TestEqual(TEXT("scratch==twin hit count"), ScratchHits.Num(), TwinHits.Num());
			for (int32 i = 0; i < FMath::Min(ScratchHits.Num(), TwinHits.Num()); ++i)
			{
				TestTrue(TEXT("scratch channel hit == baked twin hit"),
					ScratchHits[i].Index == TwinHits[i].Index &&
						ScratchHits[i].Score == TwinHits[i].Score);
			}
		}

		// The BP surface: QuerySimilarScratchChannels reaches the same result.
		TArray<FSuperFAISSHit> BpHits;
		const bool bBpOk = Subsystem->QuerySimilarScratchChannels(
			Scratch, Query, Named.Channels, 10, BpHits);
		TestTrue(TEXT("BP scratch channel query succeeds"), bBpOk);
		if (bBpOk && bTwinOk)
		{
			TestEqual(TEXT("BP scratch hit count == twin"), BpHits.Num(), TwinHits.Num());
			for (int32 i = 0; i < FMath::Min(BpHits.Num(), TwinHits.Num()); ++i)
			{
				TestTrue(TEXT("BP scratch channel hit == baked twin hit"),
					BpHits[i].Index == TwinHits[i].Index &&
						BpHits[i].Score == TwinHits[i].Score);
			}
		}

		// A raw whole-vector query on the channel scratch bank is unchanged (matches the
		// twin's whole-vector query — the "no whole-vector path changed" claim).
		FSuperFAISSQueryArgs Plain;
		Plain.K = 10;
		TArray<FSuperFAISSHit> PlainScratch, PlainTwin;
		const bool bPlainScratch = Subsystem->QueryScratch(Scratch, Query, Plain, PlainScratch);
		const bool bPlainTwin = Subsystem->QuerySync(Twin, Query, Plain, PlainTwin);
		TestTrue(TEXT("raw whole-vector scratch query works"), bPlainScratch);
		if (bPlainScratch && bPlainTwin)
		{
			for (int32 i = 0; i < FMath::Min(PlainScratch.Num(), PlainTwin.Num()); ++i)
			{
				TestTrue(TEXT("whole-vector scratch == twin hit"),
					PlainScratch[i].Index == PlainTwin[i].Index &&
						PlainScratch[i].Score == PlainTwin[i].Score);
			}
		}

		// The resolver (N1): an unknown channel name is rejected, and the accepted
		// named query is the discriminator so a not-created bank cannot mark it swept.
		FSuperFAISSQueryArgs Unknown;
		Unknown.K = 4;
		Unknown.Channels = {{TEXT("nonexistent"), 1.0f}};
		TArray<FSuperFAISSHit> UnknownHits;
		const bool bUnknownOk =
			Subsystem->QueryScratch(Scratch, Query, Unknown, UnknownHits);
		TestTrue(TEXT("unknown scratch channel rejected (valid accepted)"),
			bScratchOk && !bUnknownOk);
	}

	// A named-channel query on a single-space (channel-less) scratch bank stays
	// rejected — no table, resolved genuinely rather than by the old blanket guard.
	{
		USuperFAISSScratchBank* Plain = NewObject<USuperFAISSScratchBank>();
		TestTrue(TEXT("plain init"), Plain->Init(8, Dims,
			ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Float32));
		TArray<float> Good = ChanScratchRows(1, Dims, 0x77ull);
		int32 Index = INDEX_NONE;
		TestTrue(TEXT("plain append"), Plain->Append(Good, Index));
		FSuperFAISSQueryArgs Named;
		Named.K = 1;
		Named.Channels = {{TEXT("identity"), 1.0f}};
		TArray<FSuperFAISSHit> Hits;
		TestFalse(TEXT("named-channel query on channel-less scratch rejected"),
			Subsystem->QueryScratch(Plain, Good, Named, Hits));
	}

	return true;
}

// T-V3-U3 (cell 3, step A): per-channel scratch Decompose. A hit's per-channel
// contributions sum bitwise to the total, the total equals the per-channel scan's own
// score for that row on the snapshot, and both equal the baked twin's decomposition of
// the same row (the same append order gives the same row indices).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSChannelScratchDecomposeTest,
	"SuperFAISS.A.ChannelScratchDecompose",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSChannelScratchDecomposeTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	constexpr int32 Count = 128;
	constexpr int32 Dims = 32;
	const TArray<float> Rows = ChanScratchRows(Count, Dims, 0xDEC0DEull);
	const TArray<float> Query = ChanScratchRows(1, Dims, 0x9E11ull);
	const TArray<FSuperFAISSChannelWeight> Channels =
		{{TEXT("identity"), 1.0f}, {TEXT("appearance"), 0.25f}};

	USuperFAISSScratchBank* Scratch = NewObject<USuperFAISSScratchBank>();
	Scratch->InitWithChannels(Count, Dims, ESuperFAISSBankMetric::Cosine,
		ESuperFAISSBankQuantization::Float32, ChanNames(), {0, 16}, {16, 16}, false);
	AppendRows(*this, Scratch, Rows, Count, Dims);

	USuperFAISSVectorBank* Twin = NewObject<USuperFAISSVectorBank>();
	FString Error;
	TestTrue(TEXT("baked twin built"), Twin->InitFromSource(Rows, Count, Dims,
		ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Float32, {},
		TEXT("decomp-twin"), Error, ChanNames(), {0, 16}, {16, 16}));

	constexpr int32 RowIndex = 7;
	TArray<float> Contribs;
	float Total = 0.0f;
	const bool bOk = Subsystem->DecomposeScratchHit(
		Scratch, Query, Channels, RowIndex, Contribs, Total);
	TestTrue(TEXT("scratch decompose succeeds"), bOk);
	if (bOk)
	{
		TestEqual(TEXT("contribution count"), Contribs.Num(), 2);
		TestEqual(TEXT("contributions sum == total"),
			Contribs.Num() == 2 ? Contribs[0] + Contribs[1] : 0.0f, Total);

		// The total is the per-channel scan's own score for the row (feature oracle via
		// the baked twin's decomposition of the same index).
		TArray<float> TwinContribs;
		float TwinTotal = 0.0f;
		const bool bTwinOk = Subsystem->DecomposeHit(
			Twin, Query, Channels, RowIndex, TwinContribs, TwinTotal);
		if (TestTrue(TEXT("baked twin decompose"), bTwinOk))
		{
			TestEqual(TEXT("scratch total == baked twin total"), Total, TwinTotal);
			TestEqual(TEXT("scratch contribs == twin contribs"),
				Contribs.Num(), TwinContribs.Num());
			for (int32 i = 0; i < FMath::Min(Contribs.Num(), TwinContribs.Num()); ++i)
			{
				TestEqual(TEXT("scratch contrib == twin contrib"),
					Contribs[i], TwinContribs[i]);
			}
		}
	}

	return true;
}

// T-V3-U4 (cell 4, step A): channel-aware Freeze. Freezing a channel scratch bank
// yields a schemaVersion-2 baked channel bank that carries the channel table and is
// queryable per-channel; the brute-force end-state holds (the frozen bank's per-channel
// query equals the independently-baked twin's, not only frozen==snapshot). A freeze
// with a tombstone compacts and stays schema-2 with the surviving rows.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSChannelScratchFreezeTest,
	"SuperFAISS.A.ChannelScratchFreeze",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSChannelScratchFreezeTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	constexpr int32 Count = 96;
	constexpr int32 Dims = 32;
	const TArray<float> Rows = ChanScratchRows(Count, Dims, 0xF20E5Eull);
	const TArray<float> Query = ChanScratchRows(1, Dims, 0x9E11ull);
	const TArray<FSuperFAISSChannelWeight> Channels =
		{{TEXT("identity"), 1.0f}, {TEXT("appearance"), 0.5f}};

	// (a) No-removal freeze: the frozen bank is the whole set, so its per-channel query
	// equals the independently-baked twin's — the achievement end-state.
	{
		USuperFAISSScratchBank* Scratch = NewObject<USuperFAISSScratchBank>();
		Scratch->InitWithChannels(Count, Dims, ESuperFAISSBankMetric::Cosine,
			ESuperFAISSBankQuantization::Int8, ChanNames(), {0, 16}, {16, 16}, false);
		AppendRows(*this, Scratch, Rows, Count, Dims);

		TArray<int32> IndexMap;
		USuperFAISSVectorBank* Frozen = Scratch->Freeze(IndexMap);
		if (TestNotNull(TEXT("channel freeze returns a bank"), Frozen))
		{
			TestEqual(TEXT("frozen schemaVersion 2 (channels carried, R-V3-3)"),
				Frozen->SchemaVersion, 2);
			TestEqual(TEXT("frozen channel count"), Frozen->GetChannelCount(), 2);
			TestTrue(TEXT("frozen valid"), Frozen->IsValid());
			TestEqual(TEXT("frozen count"), Frozen->Count, Count);

			USuperFAISSVectorBank* Twin = NewObject<USuperFAISSVectorBank>();
			FString Error;
			TestTrue(TEXT("baked twin built"), Twin->InitFromSource(Rows, Count, Dims,
				ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Int8, {},
				TEXT("freeze-twin"), Error, ChanNames(), {0, 16}, {16, 16}));

			TArray<FSuperFAISSHit> FrozenHits, TwinHits;
			const bool bFrozen =
				Subsystem->QuerySimilarChannels(Frozen, Query, Channels, 10, FrozenHits);
			const bool bTwin =
				Subsystem->QuerySimilarChannels(Twin, Query, Channels, 10, TwinHits);
			TestTrue(TEXT("frozen per-channel query"), bFrozen);
			if (bFrozen && bTwin)
			{
				TestEqual(TEXT("frozen==twin hit count"), FrozenHits.Num(), TwinHits.Num());
				for (int32 i = 0; i < FMath::Min(FrozenHits.Num(), TwinHits.Num()); ++i)
				{
					TestTrue(TEXT("frozen channel hit == baked twin hit (end-state)"),
						FrozenHits[i].Index == TwinHits[i].Index &&
							FrozenHits[i].Score == TwinHits[i].Score);
				}
			}
		}
	}

	// (b) Freeze after a tombstone: compaction renumbers, the asset stays schema-2 with
	// the channel table and the surviving rows are queryable per-channel.
	{
		USuperFAISSScratchBank* Scratch = NewObject<USuperFAISSScratchBank>();
		Scratch->InitWithChannels(Count, Dims, ESuperFAISSBankMetric::Cosine,
			ESuperFAISSBankQuantization::Int8, ChanNames(), {0, 16}, {16, 16}, false);
		AppendRows(*this, Scratch, Rows, Count, Dims);
		Scratch->Remove(3);

		TArray<int32> IndexMap;
		USuperFAISSVectorBank* Frozen = Scratch->Freeze(IndexMap);
		if (TestNotNull(TEXT("compacting channel freeze returns a bank"), Frozen))
		{
			TestEqual(TEXT("compacted frozen schemaVersion 2"), Frozen->SchemaVersion, 2);
			TestEqual(TEXT("compacted frozen channel count"), Frozen->GetChannelCount(), 2);
			TestEqual(TEXT("compacted frozen count == live"), Frozen->Count, Count - 1);
			TestEqual(TEXT("victim dropped in map"),
				IndexMap.IsValidIndex(3) ? IndexMap[3] : 0, -1);
			TArray<FSuperFAISSHit> FrozenHits;
			TestTrue(TEXT("compacted frozen per-channel query"),
				Subsystem->QuerySimilarChannels(Frozen, Query, Channels, 10, FrozenHits));
		}
	}

	return true;
}

// T-V3-U-Recall (cell 6, step B — the BP half): the per-channel recall reporting
// surface (FSuperFAISSScratchRecallReport per-channel) surfaces the slot-3 core
// MeasureScratchRecallPerChannel over BP (D-V3-7). On a retention+channel Cosine int8
// scratch, the plugin's per-channel numbers equal a direct core measurement over the
// same rows and seed (the feature oracle — the number IS the core measurement).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSChannelScratchRecallTest,
	"SuperFAISS.A.ChannelScratchRecall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSChannelScratchRecallTest::RunTest(const FString& Parameters)
{
	constexpr int32 Count = 160;
	constexpr int32 Dims = 32;
	const TArray<float> Rows = ChanScratchRows(Count, Dims, 0x2ECA11ull);

	USuperFAISSScratchBank* Audited = NewObject<USuperFAISSScratchBank>();
	Audited->InitWithChannels(Count, Dims, ESuperFAISSBankMetric::Cosine,
		ESuperFAISSBankQuantization::Int8, ChanNames(), {0, 16}, {16, 16},
		/*bRetainFloats*/ true);
	AppendRows(*this, Audited, Rows, Count, Dims);

	TArray<FSuperFAISSScratchRecallReport> Reports;
	const bool bOk = Audited->MeasureRecallPerChannel(Reports);
	TestTrue(TEXT("per-channel measure succeeds (retention+channel Cosine)"), bOk);
	if (bOk)
	{
		TestEqual(TEXT("one report per channel"), Reports.Num(), 2);
		for (const FSuperFAISSScratchRecallReport& R : Reports)
		{
			TestTrue(TEXT("recall in [0,1]"), R.Recall >= 0.0f && R.Recall <= 1.0f);
			TestTrue(TEXT("live rows counted"), R.LiveRows > 0);
		}

		// Feature oracle: the plugin numbers equal a direct core measurement.
		superfaiss::Workspace CoreWs;
		superfaiss::ScratchRecallReport CoreReports[2];
		const bool bCore = Audited->Core().MeasureScratchRecallPerChannel(
			CoreWs, CoreReports, 2) == superfaiss::Status::Ok;
		if (TestTrue(TEXT("core per-channel measure"), bCore) && Reports.Num() == 2)
		{
			for (int32 C = 0; C < 2; ++C)
			{
				TestEqual(TEXT("plugin per-channel recall == core"),
					Reports[C].Recall, CoreReports[C].recall);
				TestEqual(TEXT("plugin per-channel k == core"),
					Reports[C].K, CoreReports[C].k);
			}
		}
	}

	// Reject-over-degrade: a non-retention channel bank has no float reference.
	USuperFAISSScratchBank* Plain = NewObject<USuperFAISSScratchBank>();
	Plain->InitWithChannels(Count, Dims, ESuperFAISSBankMetric::Cosine,
		ESuperFAISSBankQuantization::Int8, ChanNames(), {0, 16}, {16, 16},
		/*bRetainFloats*/ false);
	AppendRows(*this, Plain, Rows, Count, Dims);
	TArray<FSuperFAISSScratchRecallReport> None;
	TestFalse(TEXT("non-retention per-channel measure rejected"),
		Plain->MeasureRecallPerChannel(None));

	return true;
}

// T-V3-U5 (cell 7, step B — the BP half): channel-scoped analytics over BP. Each
// §22 CrossDevice reduction gains a channel-scoped form (Channel selector). A channel
// covering the whole row scores identically to the whole-vector operator (the direct
// subsystem result — an FEAT-proven reference); two different channels give different
// values (the operator truly restricts to the sub-range); an out-of-range channel is
// rejected. Read-only.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSChannelScopedAnalyticsTest,
	"SuperFAISS.A.ChannelScopedAnalytics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSChannelScopedAnalyticsTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	constexpr int32 Count = 40;
	constexpr int32 Dims = 32;
	const TArray<float> Rows = ChanScratchRows(Count, Dims, 0xA11A17ull);

	// A whole-row single-channel int8 Dot bank: channel 0 == the whole vector.
	USuperFAISSVectorBank* Whole = NewObject<USuperFAISSVectorBank>();
	FString Error;
	TestTrue(TEXT("whole-row channel bank built"), Whole->InitFromSource(Rows, Count, Dims,
		ESuperFAISSBankMetric::Dot, ESuperFAISSBankQuantization::Int8, {},
		TEXT("whole-chan"), Error, {TEXT("all")}, {0}, {Dims}));

	// A two-channel int8 Dot bank for the sub-range discrimination.
	USuperFAISSVectorBank* Split = NewObject<USuperFAISSVectorBank>();
	TestTrue(TEXT("two-channel bank built"), Split->InitFromSource(Rows, Count, Dims,
		ESuperFAISSBankMetric::Dot, ESuperFAISSBankQuantization::Int8, {},
		TEXT("split-chan"), Error, ChanNames(), {0, 16}, {16, 16}));

	TArray<int32> AllRows;
	for (int32 i = 0; i < Count; ++i)
	{
		AllRows.Add(i);
	}
	const TArray<int32> SelA = {0, 1, 2, 3, 4, 5, 6, 7};
	const TArray<int32> SelB = {8, 9, 10, 11, 12, 13, 14, 15};

	// --- Spread: whole-row channel == whole-vector subsystem spread ---
	{
		float RefMean = 0.0f;
		float RefMax = 0.0f;
		TestTrue(TEXT("whole-vector spread mean"),
			Subsystem->BankSpreadCrossDevice(Whole, AllRows, ESuperFAISSReduce::Mean, RefMean));
		TestTrue(TEXT("whole-vector spread max"),
			Subsystem->BankSpreadCrossDevice(Whole, AllRows, ESuperFAISSReduce::Max, RefMax));
		float ChanMean = 0.0f;
		float ChanMax = 0.0f;
		const bool bMean = Subsystem->BankSpreadCrossDeviceChannel(
			Whole, AllRows, ESuperFAISSReduce::Mean, 0, ChanMean);
		const bool bMax = Subsystem->BankSpreadCrossDeviceChannel(
			Whole, AllRows, ESuperFAISSReduce::Max, 0, ChanMax);
		TestTrue(TEXT("channel spread mean succeeds"), bMean);
		TestTrue(TEXT("channel spread max succeeds"), bMax);
		if (bMean)
		{
			TestEqual(TEXT("whole-row channel spread mean == whole-vector"), ChanMean, RefMean);
		}
		if (bMax)
		{
			TestEqual(TEXT("whole-row channel spread max == whole-vector"), ChanMax, RefMax);
		}
	}

	// --- CentroidDistance: whole-row channel == whole-vector subsystem ---
	{
		float Ref = 0.0f;
		TestTrue(TEXT("whole-vector centroid distance"),
			Subsystem->SetToSetDistanceCrossDevice(Whole, SelA, {}, Whole, SelB, {},
				ESuperFAISSBankMetric::Dot, Ref));
		float Chan = 0.0f;
		const bool bChan = Subsystem->SetToSetDistanceCrossDeviceChannel(
			Whole, SelA, {}, Whole, SelB, {}, ESuperFAISSBankMetric::Dot, 0, Chan);
		TestTrue(TEXT("channel centroid distance succeeds"), bChan);
		if (bChan)
		{
			TestEqual(TEXT("whole-row channel centroid == whole-vector"), Chan, Ref);
		}
	}

	// --- MeanNN / MaxNN: whole-row channel == whole-vector subsystem ---
	{
		float RefMean = 0.0f;
		float RefMax = 0.0f;
		TestTrue(TEXT("whole-vector meanNN"),
			Subsystem->MeanNearestNeighborCrossDevice(Whole, Whole, RefMean));
		TestTrue(TEXT("whole-vector maxNN"),
			Subsystem->MaxNearestNeighborCrossDevice(Whole, Whole, RefMax));
		float ChanMean = 0.0f;
		float ChanMax = 0.0f;
		const bool bMean =
			Subsystem->MeanNearestNeighborCrossDeviceChannel(Whole, Whole, 0, ChanMean);
		const bool bMax =
			Subsystem->MaxNearestNeighborCrossDeviceChannel(Whole, Whole, 0, ChanMax);
		TestTrue(TEXT("channel meanNN succeeds"), bMean);
		TestTrue(TEXT("channel maxNN succeeds"), bMax);
		if (bMean)
		{
			TestEqual(TEXT("whole-row channel meanNN == whole-vector"), ChanMean, RefMean);
		}
		if (bMax)
		{
			TestEqual(TEXT("whole-row channel maxNN == whole-vector"), ChanMax, RefMax);
		}
	}

	// --- Sub-range discrimination: the two channels of the split bank differ, and an
	// out-of-range channel is rejected (paired with a valid channel so a stub cannot
	// mark it swept) ---
	{
		float V0 = 0.0f;
		float V1 = 0.0f;
		const bool b0 = Subsystem->BankSpreadCrossDeviceChannel(
			Split, AllRows, ESuperFAISSReduce::Mean, 0, V0);
		const bool b1 = Subsystem->BankSpreadCrossDeviceChannel(
			Split, AllRows, ESuperFAISSReduce::Mean, 1, V1);
		TestTrue(TEXT("split channel 0 spread succeeds"), b0);
		TestTrue(TEXT("split channel 1 spread succeeds"), b1);
		if (b0 && b1)
		{
			TestNotEqual(TEXT("distinct channels give distinct spread"), V0, V1);
		}
		float Oob = 0.0f;
		const bool bOob = Subsystem->BankSpreadCrossDeviceChannel(
			Split, AllRows, ESuperFAISSReduce::Mean, 5, Oob);
		TestTrue(TEXT("out-of-range channel rejected (valid accepted)"), b0 && !bOob);
	}

	return true;
}

// ============================================================================
// V3.0 plugin publish-gate coverage close-out (Curie, 2026-07-13). Japp's
// publish audit (superfaiss-v3-plugin-publish-audit-2026-07-13.md) found three
// within-cell coverage gaps on the channel scratch surface — shipped behaviors
// with no test. These cells lock the ALREADY-CORRECT behavior in place; they are
// expected GREEN on the current build. A red is a genuine forwarding/validation
// finding for Hastings, not a test to relax.
// ============================================================================

// J-1 (audit cell P-9): the channels+segments-mix rejection. QueryScratch rejects
// a query carrying BOTH Args.Channels and Args.Segments on a channel scratch bank
// (SuperFAISSSubsystem.cpp, the K<=0 / dims / mix / channel-less guard). Paired
// with the valid single-form (channels-only) query as the discriminator, so a stub
// that fails every query cannot mark the rejection swept.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSChannelScratchMixRejectTest,
	"SuperFAISS.A.ChannelScratchMixReject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSChannelScratchMixRejectTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	constexpr int32 Count = 128;
	constexpr int32 Dims = 32;
	const TArray<float> Query = ChanScratchRows(1, Dims, 0x9E11ull);

	USuperFAISSScratchBank* Scratch = NewObject<USuperFAISSScratchBank>();
	Scratch->InitWithChannels(Count, Dims, ESuperFAISSBankMetric::Cosine,
		ESuperFAISSBankQuantization::Float32, ChanNames(), {0, 16}, {16, 16}, false);
	AppendRows(*this, Scratch, ChanScratchRows(Count, Dims, 0x31C0DEull), Count, Dims);

	// The valid single-form query (channels only) is accepted — the discriminator.
	FSuperFAISSQueryArgs Valid;
	Valid.K = 10;
	Valid.Channels = {{TEXT("identity"), 1.0f}, {TEXT("appearance"), 0.25f}};
	TArray<FSuperFAISSHit> ValidHits;
	const bool bValidOk = Subsystem->QueryScratch(Scratch, Query, Valid, ValidHits);
	TestTrue(TEXT("valid channels-only scratch query accepted"), bValidOk);

	// Both Channels AND Segments populated — the defined rejection.
	FSuperFAISSQueryArgs Mixed;
	Mixed.K = 10;
	Mixed.Channels = {{TEXT("identity"), 1.0f}};
	Mixed.Segments = {{16, 16, 0.25f}}; // an explicit raw range alongside the channel
	TArray<FSuperFAISSHit> MixedHits;
	const bool bMixedOk = Subsystem->QueryScratch(Scratch, Query, Mixed, MixedHits);
	TestTrue(TEXT("channels+segments mix rejected (valid accepted)"),
		bValidOk && !bMixedOk);
	TestEqual(TEXT("rejected mix returns no hits"), MixedHits.Num(), 0);

	return true;
}

// J-2 (audit cell P-25, dim-8 composition): channel scratch query × per-row bias,
// through the plugin path, for BOTH bias forms (dense RowBias sized to the snapshot,
// sparse BiasPairs). The composed ranking is asserted against (a) the baked twin's
// biased query — a full feature oracle — and (b) a hand-computed bias effect: a row
// that ranks last unbiased, biased by +Delta, ranks first with score exactly its
// unbiased score + Delta. Catches a plugin-side dropped/mis-forwarded bias term.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSChannelScratchQueryBiasTest,
	"SuperFAISS.A.ChannelScratchQueryBias",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSChannelScratchQueryBiasTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	constexpr int32 Count = 200;
	constexpr int32 Dims = 32;
	constexpr float Delta = 10.0f; // >> the composed Cosine score range (~[-1.25,1.25])
	const TArray<float> Rows = ChanScratchRows(Count, Dims, 0xB1A5ull);
	const TArray<float> Query = ChanScratchRows(1, Dims, 0x9E11ull);
	const TArray<FSuperFAISSChannelWeight> Channels =
		{{TEXT("identity"), 1.0f}, {TEXT("appearance"), 0.25f}};

	USuperFAISSScratchBank* Scratch = NewObject<USuperFAISSScratchBank>();
	Scratch->InitWithChannels(Count, Dims, ESuperFAISSBankMetric::Cosine,
		ESuperFAISSBankQuantization::Float32, ChanNames(), {0, 16}, {16, 16}, false);
	AppendRows(*this, Scratch, Rows, Count, Dims);

	USuperFAISSVectorBank* Twin = NewObject<USuperFAISSVectorBank>();
	FString Error;
	TestTrue(TEXT("baked twin built"), Twin->InitFromSource(Rows, Count, Dims,
		ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Float32, {},
		TEXT("bias-twin"), Error, ChanNames(), {0, 16}, {16, 16}));

	// Unbiased full ranking: the last hit is the lowest-scoring live row — the target
	// whose promotion to rank 0 proves the bias actually moved the ranking.
	FSuperFAISSQueryArgs Unbiased;
	Unbiased.K = Count;
	Unbiased.Channels = Channels;
	TArray<FSuperFAISSHit> AllHits;
	const bool bUnbiasedOk = Subsystem->QueryScratch(Scratch, Query, Unbiased, AllHits);
	TestTrue(TEXT("unbiased full-ranking query"), bUnbiasedOk);
	if (!bUnbiasedOk || AllHits.Num() < 2)
	{
		return true;
	}
	const int32 Target = AllHits.Last().Index;
	const float TargetUnbiasedScore = AllHits.Last().Score;
	TestTrue(TEXT("target is not already rank 0 (bias will move it)"),
		AllHits[0].Index != Target);

	// A dense bias and a sparse bias that both reward exactly the target by +Delta.
	TArray<float> DenseBias;
	DenseBias.SetNumZeroed(Count); // sized to the snapshot count (no tombstones)
	DenseBias[Target] = Delta;
	const TArray<FSuperFAISSBiasPair> SparseBias = {{Target, Delta}};

	auto CheckBiasForm = [&](const TCHAR* Label, const FSuperFAISSQueryArgs& Args)
	{
		TArray<FSuperFAISSHit> ScratchHits;
		const bool bOk = Subsystem->QueryScratch(Scratch, Query, Args, ScratchHits);
		TestTrue(*FString::Printf(TEXT("%s: biased scratch query succeeds"), Label), bOk);
		if (!bOk || ScratchHits.Num() == 0)
		{
			return;
		}
		// Hand-computed effect: target now ranks 0 with score == unbiased + Delta.
		TestEqual(*FString::Printf(TEXT("%s: target promoted to rank 0"), Label),
			ScratchHits[0].Index, Target);
		TestTrue(*FString::Printf(TEXT("%s: biased score == unbiased + Delta"), Label),
			ScratchHits[0].Score == TargetUnbiasedScore + Delta);

		// Full feature oracle: scratch biased ranking == baked twin biased ranking.
		TArray<FSuperFAISSHit> TwinHits;
		const bool bTwinOk = Subsystem->QuerySync(Twin, Query, Args, TwinHits);
		TestTrue(*FString::Printf(TEXT("%s: baked twin biased query"), Label), bTwinOk);
		if (bTwinOk)
		{
			TestEqual(*FString::Printf(TEXT("%s: scratch==twin biased hit count"), Label),
				ScratchHits.Num(), TwinHits.Num());
			for (int32 i = 0; i < FMath::Min(ScratchHits.Num(), TwinHits.Num()); ++i)
			{
				TestTrue(*FString::Printf(TEXT("%s: scratch biased hit == twin hit"), Label),
					ScratchHits[i].Index == TwinHits[i].Index &&
						ScratchHits[i].Score == TwinHits[i].Score);
			}
		}
	};

	FSuperFAISSQueryArgs DenseArgs;
	DenseArgs.K = 10;
	DenseArgs.Channels = Channels;
	DenseArgs.RowBias = DenseBias;
	CheckBiasForm(TEXT("dense RowBias"), DenseArgs);

	FSuperFAISSQueryArgs SparseArgs;
	SparseArgs.K = 10;
	SparseArgs.Channels = Channels;
	SparseArgs.BiasPairs = SparseBias;
	CheckBiasForm(TEXT("sparse BiasPairs"), SparseArgs);

	// A dense bias sized to anything but the snapshot count is rejection, never a
	// silent misalignment (the T-055 N2 contract on the scratch path).
	FSuperFAISSQueryArgs BadDense;
	BadDense.K = 10;
	BadDense.Channels = Channels;
	BadDense.RowBias.SetNumZeroed(Count - 1);
	TArray<FSuperFAISSHit> BadHits;
	TestFalse(TEXT("mis-sized dense RowBias rejected"),
		Subsystem->QueryScratch(Scratch, Query, BadDense, BadHits));

	return true;
}

// J-2 (audit cell P-25, dim-8 composition): channel scratch query × bCrossDeviceExact,
// through the plugin path, against the cross-device baked twin. Int8 Cosine channel
// bank; the scratch cross-device channel ranking must equal the baked twin's, hit for
// hit, score for score (bitwise) — the cross-device contract carried through the
// scratch wrapper. Catches a plugin-side wrong exactness branch on the channel path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSChannelScratchQueryCrossDeviceTest,
	"SuperFAISS.A.ChannelScratchQueryCrossDevice",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSChannelScratchQueryCrossDeviceTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	constexpr int32 Count = 200;
	constexpr int32 Dims = 32;
	const TArray<float> Rows = ChanScratchRows(Count, Dims, 0xC205Deull);
	const TArray<float> Query = ChanScratchRows(1, Dims, 0x9E11ull);
	const TArray<FSuperFAISSChannelWeight> Channels =
		{{TEXT("identity"), 1.0f}, {TEXT("appearance"), 0.25f}};

	// Int8 is required for cross-device exactness (an f32 bank fails the query).
	USuperFAISSScratchBank* Scratch = NewObject<USuperFAISSScratchBank>();
	Scratch->InitWithChannels(Count, Dims, ESuperFAISSBankMetric::Cosine,
		ESuperFAISSBankQuantization::Int8, ChanNames(), {0, 16}, {16, 16}, false);
	AppendRows(*this, Scratch, Rows, Count, Dims);

	USuperFAISSVectorBank* Twin = NewObject<USuperFAISSVectorBank>();
	FString Error;
	TestTrue(TEXT("baked cross-device twin built"), Twin->InitFromSource(Rows, Count, Dims,
		ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Int8, {},
		TEXT("xd-twin"), Error, ChanNames(), {0, 16}, {16, 16}));

	FSuperFAISSQueryArgs Args;
	Args.K = 10;
	Args.Channels = Channels;
	Args.bCrossDeviceExact = true;

	TArray<FSuperFAISSHit> ScratchHits;
	const bool bScratchOk = Subsystem->QueryScratch(Scratch, Query, Args, ScratchHits);
	TestTrue(TEXT("cross-device channel scratch query succeeds"), bScratchOk);
	TestTrue(TEXT("cross-device scratch query returns hits"), ScratchHits.Num() > 0);

	TArray<FSuperFAISSHit> TwinHits;
	const bool bTwinOk = Subsystem->QuerySync(Twin, Query, Args, TwinHits);
	TestTrue(TEXT("baked cross-device twin channel query"), bTwinOk);
	if (bScratchOk && bTwinOk)
	{
		TestEqual(TEXT("xd scratch==twin hit count"), ScratchHits.Num(), TwinHits.Num());
		for (int32 i = 0; i < FMath::Min(ScratchHits.Num(), TwinHits.Num()); ++i)
		{
			TestTrue(TEXT("xd scratch channel hit == baked twin hit (bitwise)"),
				ScratchHits[i].Index == TwinHits[i].Index &&
					ScratchHits[i].Score == TwinHits[i].Score);
		}
	}

	// An f32 channel scratch bank rejects the cross-device query (int8-only contract),
	// paired with the accepted int8 form so the rejection is a genuine discrimination.
	USuperFAISSScratchBank* Float = NewObject<USuperFAISSScratchBank>();
	Float->InitWithChannels(Count, Dims, ESuperFAISSBankMetric::Cosine,
		ESuperFAISSBankQuantization::Float32, ChanNames(), {0, 16}, {16, 16}, false);
	AppendRows(*this, Float, Rows, Count, Dims);
	TArray<FSuperFAISSHit> FloatHits;
	const bool bFloatOk = Subsystem->QueryScratch(Float, Query, Args, FloatHits);
	TestTrue(TEXT("f32 bank rejects cross-device query (int8 accepted)"),
		bScratchOk && !bFloatOk);

	return true;
}

// J-2 (audit cell P-25, dim-8 composition): channel scratch query × tombstone
// exclusion, through the plugin path. Removing the unbiased rank-0 row must drop it
// from the results and promote the old rank-1 to rank-0 — the tombstone OR'd into the
// scan's exclusion set on the channel path. Feature oracle: the post-removal top-k is
// the pre-removal top-k with the victim excised and every survivor's score unchanged.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSChannelScratchQueryTombstoneTest,
	"SuperFAISS.A.ChannelScratchQueryTombstone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSChannelScratchQueryTombstoneTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	constexpr int32 Count = 200;
	constexpr int32 Dims = 32;
	const TArray<float> Query = ChanScratchRows(1, Dims, 0x9E11ull);
	const TArray<FSuperFAISSChannelWeight> Channels =
		{{TEXT("identity"), 1.0f}, {TEXT("appearance"), 0.25f}};

	USuperFAISSScratchBank* Scratch = NewObject<USuperFAISSScratchBank>();
	Scratch->InitWithChannels(Count, Dims, ESuperFAISSBankMetric::Cosine,
		ESuperFAISSBankQuantization::Float32, ChanNames(), {0, 16}, {16, 16}, false);
	AppendRows(*this, Scratch, ChanScratchRows(Count, Dims, 0x70B85ull), Count, Dims);

	FSuperFAISSQueryArgs Args;
	Args.K = 10;
	Args.Channels = Channels;

	TArray<FSuperFAISSHit> BaseHits;
	const bool bBaseOk = Subsystem->QueryScratch(Scratch, Query, Args, BaseHits);
	TestTrue(TEXT("pre-removal channel query"), bBaseOk);
	if (!bBaseOk || BaseHits.Num() < 3)
	{
		return true;
	}
	const int32 Victim = BaseHits[0].Index;

	// Remove the top-ranked row; deletion is exclusion (the tombstone is OR'd in).
	TestTrue(TEXT("victim removed"), Scratch->Remove(Victim));

	TArray<FSuperFAISSHit> AfterHits;
	const bool bAfterOk = Subsystem->QueryScratch(Scratch, Query, Args, AfterHits);
	TestTrue(TEXT("post-removal channel query"), bAfterOk);
	if (!bAfterOk)
	{
		return true;
	}

	// The victim is absent from the post-removal results.
	bool bVictimPresent = false;
	for (const FSuperFAISSHit& Hit : AfterHits)
	{
		bVictimPresent |= (Hit.Index == Victim);
	}
	TestFalse(TEXT("removed row absent from channel query results"), bVictimPresent);

	// The post-removal top-k is the pre-removal ranking shifted up by one: survivor
	// index and score are unchanged (a genuine tombstone-exclusion feature oracle).
	const int32 Compare = FMath::Min(AfterHits.Num(), BaseHits.Num() - 1);
	for (int32 i = 0; i < Compare; ++i)
	{
		TestTrue(TEXT("survivor promoted with unchanged index+score"),
			AfterHits[i].Index == BaseHits[i + 1].Index &&
				AfterHits[i].Score == BaseHits[i + 1].Score);
	}

	return true;
}

// J-2 (audit cell P-25, dim-8 composition): DecomposeScratchHit × bias and
// × bCrossDeviceExact, through the plugin path. Two contracts, per the core kernels.h
// note surfaced in the header:
//   PerDevice — contributions sum bitwise to OutTotal; RowBias is the visible separate
//               term (contributions + RowBias == OutTotal).
//   CrossDevice — OutTotal is the scan's double-chain score (bitwise equal to a
//               bCrossDeviceExact channel query's score for the row); the float re-sum
//               of the contributions is NOT asserted to equal it (a PerDevice property).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSChannelScratchDecomposeComposeTest,
	"SuperFAISS.A.ChannelScratchDecomposeCompose",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSChannelScratchDecomposeComposeTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	constexpr int32 Count = 128;
	constexpr int32 Dims = 32;
	constexpr int32 RowIndex = 7;
	constexpr float Bias = 3.5f;
	const TArray<float> Rows = ChanScratchRows(Count, Dims, 0xDEC0DE2ull);
	const TArray<float> Query = ChanScratchRows(1, Dims, 0x9E11ull);
	const TArray<FSuperFAISSChannelWeight> Channels =
		{{TEXT("identity"), 1.0f}, {TEXT("appearance"), 0.25f}};

	// Int8 so the same bank serves both the PerDevice and CrossDevice contracts.
	USuperFAISSScratchBank* Scratch = NewObject<USuperFAISSScratchBank>();
	Scratch->InitWithChannels(Count, Dims, ESuperFAISSBankMetric::Cosine,
		ESuperFAISSBankQuantization::Int8, ChanNames(), {0, 16}, {16, 16}, false);
	AppendRows(*this, Scratch, Rows, Count, Dims);

	// --- PerDevice × bias ---
	{
		TArray<float> Contribs0;
		float Total0 = 0.0f;
		const bool bOk0 = Subsystem->DecomposeScratchHit(
			Scratch, Query, Channels, RowIndex, Contribs0, Total0,
			/*RowBias*/ 0.0f, /*bCrossDeviceExact*/ false);
		TestTrue(TEXT("PerDevice unbiased decompose succeeds"), bOk0);

		TArray<float> ContribsB;
		float TotalB = 0.0f;
		const bool bOkB = Subsystem->DecomposeScratchHit(
			Scratch, Query, Channels, RowIndex, ContribsB, TotalB,
			/*RowBias*/ Bias, /*bCrossDeviceExact*/ false);
		TestTrue(TEXT("PerDevice biased decompose succeeds"), bOkB);

		if (bOk0 && bOkB)
		{
			TestEqual(TEXT("PerDevice contribution count"), Contribs0.Num(), 2);
			// Contributions sum bitwise to the unbiased total.
			TestTrue(TEXT("PerDevice contribs sum == unbiased total"),
				Contribs0.Num() == 2 && Contribs0[0] + Contribs0[1] == Total0);
			// Bias is the visible separate term: same contribs, total shifted by bias.
			TestEqual(TEXT("biased contribs == unbiased contribs"),
				ContribsB.Num(), Contribs0.Num());
			for (int32 i = 0; i < FMath::Min(ContribsB.Num(), Contribs0.Num()); ++i)
			{
				TestTrue(TEXT("bias leaves contributions unchanged"),
					ContribsB[i] == Contribs0[i]);
			}
			TestTrue(TEXT("PerDevice contribs + RowBias == biased total"),
				ContribsB.Num() == 2 && ContribsB[0] + ContribsB[1] + Bias == TotalB);
			TestTrue(TEXT("PerDevice biased total == unbiased total + bias"),
				TotalB == Total0 + Bias);
		}
	}

	// --- CrossDevice: OutTotal == the bCrossDeviceExact channel query's scan score ---
	{
		TArray<float> ContribsXd;
		float TotalXd = 0.0f;
		const bool bOkXd = Subsystem->DecomposeScratchHit(
			Scratch, Query, Channels, RowIndex, ContribsXd, TotalXd,
			/*RowBias*/ 0.0f, /*bCrossDeviceExact*/ true);
		TestTrue(TEXT("CrossDevice decompose succeeds"), bOkXd);

		// The scan score: a bCrossDeviceExact channel query over all live rows carries
		// RowIndex's score, which OutTotal must equal bitwise (kernels.h contract).
		FSuperFAISSQueryArgs XdArgs;
		XdArgs.K = Count;
		XdArgs.Channels = Channels;
		XdArgs.bCrossDeviceExact = true;
		TArray<FSuperFAISSHit> XdHits;
		const bool bXdQuery = Subsystem->QueryScratch(Scratch, Query, XdArgs, XdHits);
		TestTrue(TEXT("cross-device full channel query"), bXdQuery);

		float ScanScore = 0.0f;
		bool bFound = false;
		for (const FSuperFAISSHit& Hit : XdHits)
		{
			if (Hit.Index == RowIndex)
			{
				ScanScore = Hit.Score;
				bFound = true;
				break;
			}
		}
		TestTrue(TEXT("RowIndex present in cross-device query hits"), bFound);
		if (bOkXd && bFound)
		{
			TestTrue(TEXT("CrossDevice decompose OutTotal == scan score (bitwise)"),
				TotalXd == ScanScore);
		}
	}

	return true;
}

// J-3 (audit cell P-10): the zero-norm Cosine query sub-vector rejection. A named
// channel query whose sub-vector over a NONZERO-weight channel is all-zero is a query
// validation error (ZeroNormQuery, the core ValidateSegments law), surfaced through
// the plugin scratch path: QueryScratch must return false, not score it silently.
// Paired with the same query carrying a nonzero identity sub-vector, which is accepted
// — the discriminator. A GREEN here confirms the core rejection reaches the plugin
// path; a RED (silent accept) is a finding for Hastings (Poirot's J-3 lead).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSChannelScratchZeroNormTest,
	"SuperFAISS.A.ChannelScratchZeroNorm",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSChannelScratchZeroNormTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	constexpr int32 Count = 64;
	constexpr int32 Dims = 32;
	const TArray<float> Rows = ChanScratchRows(Count, Dims, 0x2E80ull);
	const TArray<FSuperFAISSChannelWeight> Channels =
		{{TEXT("identity"), 1.0f}, {TEXT("appearance"), 0.25f}};

	USuperFAISSScratchBank* Scratch = NewObject<USuperFAISSScratchBank>();
	Scratch->InitWithChannels(Count, Dims, ESuperFAISSBankMetric::Cosine,
		ESuperFAISSBankQuantization::Float32, ChanNames(), {0, 16}, {16, 16}, false);
	AppendRows(*this, Scratch, Rows, Count, Dims);

	// A well-formed query (both sub-vectors nonzero) is accepted — the discriminator.
	const TArray<float> ValidQuery = ChanScratchRows(1, Dims, 0x9E11ull);
	FSuperFAISSQueryArgs Args;
	Args.K = 8;
	Args.Channels = Channels;
	TArray<FSuperFAISSHit> ValidHits;
	const bool bValidOk = Subsystem->QueryScratch(Scratch, ValidQuery, Args, ValidHits);
	TestTrue(TEXT("nonzero-subvector channel query accepted"), bValidOk);

	// The identity sub-vector [0,16) is all zero while its channel weight is 1.0; the
	// appearance sub-vector [16,32) is nonzero, so the whole query is not degenerate —
	// isolating the per-segment zero-norm law to the identity channel.
	TArray<float> ZeroNormQuery = ValidQuery;
	for (int32 J = 0; J < 16; ++J)
	{
		ZeroNormQuery[J] = 0.0f;
	}
	TArray<FSuperFAISSHit> ZeroHits;
	const bool bZeroOk = Subsystem->QueryScratch(Scratch, ZeroNormQuery, Args, ZeroHits);
	TestTrue(TEXT("zero-norm nonzero-weight sub-vector rejected (valid accepted)"),
		bValidOk && !bZeroOk);

	// The BP surface rejects it identically (no silent score through the wrapper).
	TArray<FSuperFAISSHit> BpZeroHits;
	const bool bBpZeroOk = Subsystem->QuerySimilarScratchChannels(
		Scratch, ZeroNormQuery, Channels, 8, BpZeroHits);
	TestTrue(TEXT("BP zero-norm sub-vector rejected"), !bBpZeroOk);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
