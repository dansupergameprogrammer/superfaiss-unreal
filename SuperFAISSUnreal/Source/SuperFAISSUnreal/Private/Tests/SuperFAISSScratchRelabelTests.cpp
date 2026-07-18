// Scratch-bank Relabel tests (T-099 slot 4, V3.1 mutable channel vocabulary on the
// plugin surface). The core Relabel is green and tag-shipped; this suite proves the
// plugin wrapper — USuperFAISSScratchBank::Relabel — surfaces it correctly: the two-step
// gate (core-surface parity, then a named-channel query over the NEW partition returns the
// expected neighbours vs an independent brute force), the §24.7 parity oracle (a relabeled
// bank == a fresh InitWithChannels of the new table over the same rows), reject-over-
// degrade (a malformed table leaves the old table and host-side names intact), promote /
// demote, and the generation advance that makes a pre-relabel recall report read stale.
//
// The FEAT oracle (SubRangeCosine) is computed from the raw pre-append floats, grounded in
// the definition of cosine similarity — never a recode of the scratch/core scoring path.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "SuperFAISSScratchBank.h"
#include "SuperFAISSSubsystem.h"

namespace
{
	TArray<float> RelabelRows(int32 Count, int32 Dims, uint64 Seed)
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

	// Cosine of two sub-vectors from the definition (dot / (||a|| * ||b||)); per-channel
	// cosine is invariant to a uniform positive rescale of either full vector, so the raw
	// pre-append rows are the correct expectation for the bank's per-channel score.
	double SubRangeCosine(const float* A, const float* B, int32 Offset, int32 Length)
	{
		double Dot = 0.0, SqA = 0.0, SqB = 0.0;
		for (int32 i = 0; i < Length; ++i)
		{
			const double Av = A[Offset + i];
			const double Bv = B[Offset + i];
			Dot += Av * Bv;
			SqA += Av * Av;
			SqB += Bv * Bv;
		}
		const double Denom = FMath::Sqrt(SqA) * FMath::Sqrt(SqB);
		return Denom > 0.0 ? Dot / Denom : 0.0;
	}

	USuperFAISSScratchBank* MakeChannelBank(int32 Cap, int32 Dims,
		const TArray<FName>& Names, const TArray<int32>& Offsets,
		const TArray<int32>& Lengths, const TArray<float>& Rows, int32 Count,
		bool bRetain = false)
	{
		USuperFAISSScratchBank* Bank = NewObject<USuperFAISSScratchBank>();
		if (!Bank->InitWithChannels(Cap, Dims, ESuperFAISSBankMetric::Cosine,
				ESuperFAISSBankQuantization::Float32, Names, Offsets, Lengths, bRetain))
		{
			return nullptr;
		}
		TArray<float> Row;
		Row.SetNumUninitialized(Dims);
		int32 Index = INDEX_NONE;
		for (int32 R = 0; R < Count; ++R)
		{
			FMemory::Memcpy(Row.GetData(), Rows.GetData() + R * Dims, Dims * sizeof(float));
			Bank->Append(Row, Index);
		}
		return Bank;
	}
}

// -----------------------------------------------------------------------------------
// Cell 1 — two-step gate. Step 1: Relabel changes the observable channel surface
// (count/index). Step 2: a named-channel query over a channel of the NEW partition
// returns the full ranking an independent brute force over that new sub-range predicts.
// -----------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSScratchRelabelGateTest,
	"SuperFAISS.A.ScratchRelabelGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSScratchRelabelGateTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	constexpr int32 Count = 40;
	constexpr int32 Dims = 32;
	const TArray<float> Rows = RelabelRows(Count, Dims, 0x0EA13A1Full);
	const TArray<float> Query = RelabelRows(1, Dims, 0xFEA7C0DEull);

	// Old partition: two 16-wide channels.
	USuperFAISSScratchBank* Bank = MakeChannelBank(Count, Dims,
		{TEXT("identity"), TEXT("appearance")}, {0, 16}, {16, 16}, Rows, Count);
	if (!TestNotNull(TEXT("bank"), Bank))
	{
		return true;
	}
	TestEqual(TEXT("old channel count"), Bank->GetChannelCount(), 2);

	// New partition: three channels with different boundaries (a count-up + boundary move).
	const TArray<FName> NewNames = {TEXT("head"), TEXT("mid"), TEXT("tail")};
	const TArray<int32> NewOffsets = {0, 8, 16};
	const TArray<int32> NewLengths = {8, 8, 16};
	TestTrue(TEXT("Relabel succeeds"), Bank->Relabel(NewNames, NewOffsets, NewLengths));

	// Step 1 — the surface moved.
	TestEqual(TEXT("new channel count"), Bank->GetChannelCount(), 3);
	TestEqual(TEXT("head index"), Bank->GetChannelIndex(TEXT("head")), 0);
	TestEqual(TEXT("mid index"), Bank->GetChannelIndex(TEXT("mid")), 1);
	TestEqual(TEXT("tail index"), Bank->GetChannelIndex(TEXT("tail")), 2);
	TestEqual(TEXT("old name gone"), Bank->GetChannelIndex(TEXT("identity")), INDEX_NONE);

	// Step 2 — a named query over the new "tail" channel [16,32) matches an independent
	// brute force over that new sub-range, full ranking (order included).
	TArray<int32> ExpectedOrder;
	TArray<double> ExpectedScore;
	ExpectedScore.SetNumUninitialized(Count);
	for (int32 R = 0; R < Count; ++R)
	{
		ExpectedScore[R] = SubRangeCosine(Rows.GetData() + R * Dims, Query.GetData(), 16, 16);
		ExpectedOrder.Add(R);
	}
	ExpectedOrder.Sort([&ExpectedScore](int32 A, int32 B) { return ExpectedScore[A] > ExpectedScore[B]; });

	FSuperFAISSQueryArgs Args;
	Args.K = Count;
	Args.Channels = {{TEXT("tail"), 1.0f}};
	TArray<FSuperFAISSHit> Hits;
	TestTrue(TEXT("named query over new partition"), Subsystem->QueryScratch(Bank, Query, Args, Hits));
	TestEqual(TEXT("hit count"), Hits.Num(), Count);
	for (int32 i = 0; i < FMath::Min(Hits.Num(), Count); ++i)
	{
		TestEqual(FString::Printf(TEXT("rank %d matches brute force over new sub-range"), i),
			Hits[i].Index, ExpectedOrder[i]);
		TestTrue(FString::Printf(TEXT("rank %d score matches brute force"), i),
			FMath::IsNearlyEqual(Hits[i].Score,
				static_cast<float>(ExpectedScore[ExpectedOrder[i]]), 1e-3f));
	}

	return true;
}

// -----------------------------------------------------------------------------------
// Cell 2 — parity oracle (§24.7): a bank Init'd on the OLD table then Relabel'd to the NEW
// table scores identically to a bank Init'd DIRECTLY on the new table, over the same rows.
// A relabeled bank is a fresh bank under the new vocabulary — the whole point of the op.
// -----------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSScratchRelabelParityTest,
	"SuperFAISS.A.ScratchRelabelParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSScratchRelabelParityTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	constexpr int32 Count = 50;
	constexpr int32 Dims = 32;
	const TArray<float> Rows = RelabelRows(Count, Dims, 0x9A71C0DEull);
	const TArray<float> Query = RelabelRows(1, Dims, 0x5A1AD00Dull);

	const TArray<FName> NewNames = {TEXT("a"), TEXT("b"), TEXT("c")};
	const TArray<int32> NewOffsets = {0, 8, 24};
	const TArray<int32> NewLengths = {8, 16, 8};

	// Relabeled: born on a different two-channel table, moved to the new three-channel one.
	USuperFAISSScratchBank* Relabeled = MakeChannelBank(Count, Dims,
		{TEXT("x"), TEXT("y")}, {0, 16}, {16, 16}, Rows, Count);
	if (!TestNotNull(TEXT("relabeled bank"), Relabeled))
	{
		return true;
	}
	TestTrue(TEXT("Relabel to new table"), Relabeled->Relabel(NewNames, NewOffsets, NewLengths));

	// Fresh: born directly on the new table, same rows.
	USuperFAISSScratchBank* Fresh = MakeChannelBank(Count, Dims,
		NewNames, NewOffsets, NewLengths, Rows, Count);
	if (!TestNotNull(TEXT("fresh bank"), Fresh))
	{
		return true;
	}

	FSuperFAISSQueryArgs Args;
	Args.K = Count;
	Args.Channels = {{TEXT("a"), 1.0f}, {TEXT("b"), 0.5f}, {TEXT("c"), 0.25f}};
	TArray<FSuperFAISSHit> RelabeledHits, FreshHits;
	TestTrue(TEXT("relabeled query"), Subsystem->QueryScratch(Relabeled, Query, Args, RelabeledHits));
	TestTrue(TEXT("fresh query"), Subsystem->QueryScratch(Fresh, Query, Args, FreshHits));

	TestEqual(TEXT("relabeled==fresh hit count"), RelabeledHits.Num(), FreshHits.Num());
	for (int32 i = 0; i < FMath::Min(RelabeledHits.Num(), FreshHits.Num()); ++i)
	{
		TestEqual(FString::Printf(TEXT("relabeled==fresh rank %d index"), i),
			RelabeledHits[i].Index, FreshHits[i].Index);
		TestTrue(FString::Printf(TEXT("relabeled==fresh rank %d score"), i),
			FMath::IsNearlyEqual(RelabeledHits[i].Score, FreshHits[i].Score, 1e-4f));
	}

	return true;
}

// -----------------------------------------------------------------------------------
// Cell 3 — reject-over-degrade. A malformed Relabel (overlap, off-grid, duplicate names,
// mismatched array lengths) returns false and leaves the bank EXACTLY under the old table:
// the old names still resolve, and a query over the old partition still returns the same
// ranking it did before the failed attempt.
// -----------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSScratchRelabelRejectTest,
	"SuperFAISS.A.ScratchRelabelReject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSScratchRelabelRejectTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	constexpr int32 Count = 32;
	constexpr int32 Dims = 32;
	const TArray<float> Rows = RelabelRows(Count, Dims, 0xBAD70B1Eull);
	const TArray<float> Query = RelabelRows(1, Dims, 0x600D5EEDull);

	USuperFAISSScratchBank* Bank = MakeChannelBank(Count, Dims,
		{TEXT("identity"), TEXT("appearance")}, {0, 16}, {16, 16}, Rows, Count);
	if (!TestNotNull(TEXT("bank"), Bank))
	{
		return true;
	}

	// The reference ranking over the old "identity" channel, before any failed relabel.
	FSuperFAISSQueryArgs Args;
	Args.K = Count;
	Args.Channels = {{TEXT("identity"), 1.0f}};
	TArray<FSuperFAISSHit> Before;
	TestTrue(TEXT("baseline query"), Subsystem->QueryScratch(Bank, Query, Args, Before));

	// Overlap: [0,16) and [8,16) overlap — the core rejects the range.
	TestFalse(TEXT("overlap relabel rejected"),
		Bank->Relabel({TEXT("a"), TEXT("b")}, {0, 8}, {16, 8}));
	// Off-grid: offset 2 is not on the Float32 element grid.
	TestFalse(TEXT("off-grid relabel rejected"),
		Bank->Relabel({TEXT("a")}, {2}, {8}));
	// Duplicate names: host-side rejection.
	TestFalse(TEXT("duplicate-name relabel rejected"),
		Bank->Relabel({TEXT("a"), TEXT("a")}, {0, 16}, {16, 16}));
	// Mismatched array lengths: host-side rejection.
	TestFalse(TEXT("mismatched-array relabel rejected"),
		Bank->Relabel({TEXT("a"), TEXT("b")}, {0}, {16, 16}));

	// The old table survived every rejection intact.
	TestEqual(TEXT("channel count unchanged"), Bank->GetChannelCount(), 2);
	TestEqual(TEXT("identity still resolves"), Bank->GetChannelIndex(TEXT("identity")), 0);
	TestEqual(TEXT("appearance still resolves"), Bank->GetChannelIndex(TEXT("appearance")), 1);

	TArray<FSuperFAISSHit> After;
	TestTrue(TEXT("query over old partition still works"),
		Subsystem->QueryScratch(Bank, Query, Args, After));
	TestEqual(TEXT("ranking unchanged after rejections"), After.Num(), Before.Num());
	for (int32 i = 0; i < FMath::Min(After.Num(), Before.Num()); ++i)
	{
		TestTrue(FString::Printf(TEXT("hit %d identical to baseline"), i),
			After[i].Index == Before[i].Index && After[i].Score == Before[i].Score);
	}

	return true;
}

// -----------------------------------------------------------------------------------
// Cell 4 — promote (channel-less -> channels) and demote (channels -> single-space). The
// realloc-and-swap handles both uniformly; a demote is an empty table.
// -----------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSScratchRelabelPromoteDemoteTest,
	"SuperFAISS.A.ScratchRelabelPromoteDemote",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSScratchRelabelPromoteDemoteTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	constexpr int32 Count = 32;
	constexpr int32 Dims = 32;
	const TArray<float> Rows = RelabelRows(Count, Dims, 0x9707E77Eull);
	const TArray<float> Query = RelabelRows(1, Dims, 0xC0FFEEull);

	// Promote: channel-less bank -> two channels.
	{
		USuperFAISSScratchBank* Bank = NewObject<USuperFAISSScratchBank>();
		TestTrue(TEXT("channel-less init"),
			Bank->Init(Count, Dims, ESuperFAISSBankMetric::Cosine,
				ESuperFAISSBankQuantization::Float32));
		TArray<float> Row;
		Row.SetNumUninitialized(Dims);
		int32 Index = INDEX_NONE;
		for (int32 R = 0; R < Count; ++R)
		{
			FMemory::Memcpy(Row.GetData(), Rows.GetData() + R * Dims, Dims * sizeof(float));
			Bank->Append(Row, Index);
		}
		TestEqual(TEXT("no channels before promote"), Bank->GetChannelCount(), 0);
		TestTrue(TEXT("promote to channels"),
			Bank->Relabel({TEXT("left"), TEXT("right")}, {0, 16}, {16, 16}));
		TestEqual(TEXT("channels after promote"), Bank->GetChannelCount(), 2);

		FSuperFAISSQueryArgs Args;
		Args.K = 4;
		Args.Channels = {{TEXT("left"), 1.0f}};
		TArray<FSuperFAISSHit> Hits;
		TestTrue(TEXT("named query works after promote"),
			Subsystem->QueryScratch(Bank, Query, Args, Hits));
		TestTrue(TEXT("promote query returns hits"), Hits.Num() > 0);
	}

	// Demote: two channels -> single-space (empty table).
	{
		USuperFAISSScratchBank* Bank = MakeChannelBank(Count, Dims,
			{TEXT("left"), TEXT("right")}, {0, 16}, {16, 16}, Rows, Count);
		if (!TestNotNull(TEXT("bank"), Bank))
		{
			return true;
		}
		TestTrue(TEXT("demote to single-space"),
			Bank->Relabel(TArray<FName>{}, TArray<int32>{}, TArray<int32>{}));
		TestEqual(TEXT("no channels after demote"), Bank->GetChannelCount(), 0);
		TestEqual(TEXT("old name gone after demote"),
			Bank->GetChannelIndex(TEXT("left")), INDEX_NONE);

		// A named-channel query is now rejected (no channel table); the whole-vector query works.
		FSuperFAISSQueryArgs Named;
		Named.K = 4;
		Named.Channels = {{TEXT("left"), 1.0f}};
		TArray<FSuperFAISSHit> None;
		TestFalse(TEXT("named query rejected after demote"),
			Subsystem->QueryScratch(Bank, Query, Named, None));

		TArray<FSuperFAISSHit> WholeHits;
		TestTrue(TEXT("whole-vector query works after demote"),
			Subsystem->QuerySimilarScratch(Bank, Query, 4, WholeHits));
		TestTrue(TEXT("demote whole-vector returns hits"), WholeHits.Num() > 0);
	}

	return true;
}

// -----------------------------------------------------------------------------------
// Cell 5 — generation advance: a whole-vector recall report taken before a Relabel reads
// STALE afterward (the partition moved under it). Relabel is a mutation and advances the
// generation only forward.
// -----------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSScratchRelabelStaleReportTest,
	"SuperFAISS.A.ScratchRelabelStaleReport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSScratchRelabelStaleReportTest::RunTest(const FString& Parameters)
{
	constexpr int32 Count = 160;
	constexpr int32 Dims = 32;
	const TArray<float> Rows = RelabelRows(Count, Dims, 0x57A1E00Dull);

	USuperFAISSScratchBank* Bank = MakeChannelBank(Count, Dims,
		{TEXT("identity"), TEXT("appearance")}, {0, 16}, {16, 16}, Rows, Count,
		/*bRetain=*/true);
	if (!TestNotNull(TEXT("bank"), Bank))
	{
		return true;
	}

	FSuperFAISSScratchRecallReport Report;
	TestTrue(TEXT("whole-vector recall measured"), Bank->MeasureRecall(Report));
	TestFalse(TEXT("report current before relabel"), Bank->IsRecallReportStale(Report));

	TestTrue(TEXT("relabel"), Bank->Relabel({TEXT("a")}, {0}, {32}));
	TestTrue(TEXT("report stale after relabel"), Bank->IsRecallReportStale(Report));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
