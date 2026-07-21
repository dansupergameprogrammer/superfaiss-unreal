// Scratch-bank channel tests (T-099 slot 5, red suite authored against the
// red-scaffold stub): the scratch wrapper gets a named-channel query surface it has
// none of today. Mirrors SuperFAISSChannelTests.cpp's baked-path idiom (InitFromSource
// with a channel table, named-channel query, named==raw-range equivalence, malformed-
// table rejection matrix) and SuperFAISSScratchTests.cpp's scratch setup idiom
// (Init/Append/QueryScratch through the subsystem). Adds one FEAT oracle the baked
// suite does not need in this shape: an INDEPENDENT brute-force cosine over the
// channel's own sub-range, computed from the raw pre-append floats and grounded in
// the definition of cosine similarity — not a recode of the scratch/core scoring path.
//
// Red today, for two independent reasons the kickoff names:
//   (1) USuperFAISSScratchBank::InitWithChannels is a stub that always returns false
//       (SuperFAISSScratchBank.cpp) — every test below that depends on a channel-
//       carrying bank existing fails at that first TestTrue.
//   (2) USuperFAISSSubsystem::QueryScratch hard-rejects any query with
//       Args.Channels.Num() > 0 (SuperFAISSSubsystem.cpp:521-523), independently of
//       whether the bank has a channel table — so even a hypothetically-successful
//       InitWithChannels could not be queried by name today.
// Green work (T-099 slot 5) is expected to make both reasons go away together.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "SuperFAISSScratchBank.h"
#include "SuperFAISSSubsystem.h"

namespace
{
	// Same deterministic xorshift-derived float generator the sibling scratch/channel
	// test files use (SuperFAISSScratchTests.cpp::ScratchRows,
	// SuperFAISSChannelTests.cpp::ChannelRows) — reproduced here (anonymous-namespace
	// scoped per translation unit) so this file's fixtures are self-contained and
	// deterministic under a fixed seed, independent of any other test file's helper.
	TArray<float> ScratchChannelRows(int32 Count, int32 Dims, uint64 Seed)
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

	// The FEAT oracle's ground truth: cosine similarity of two sub-vectors, computed
	// directly from the definition (dot / (||a|| * ||b||)). Deliberately NOT a recode
	// of the scratch/core scoring path (no quantization, no whole-row normalization,
	// no channel-table lookup) — it reads raw floats at [Offset, Offset+Length) out of
	// two flat arrays and nothing else. Per-channel cosine is invariant to a uniform
	// positive rescale of either full vector (e.g. the whole-row Cosine normalization
	// scratch Append applies), so this oracle can be computed on the PRE-append raw
	// rows and still be the correct expectation for the bank's per-channel score.
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
}

// -----------------------------------------------------------------------------------
// Cell 1 — channel-carrying init + named query returns the expected neighbour.
// Oracle: hand-derived. Four rows with orthogonal per-channel basis subvectors; a
// query aligned with row 0's "identity" subvector must score exactly 1.0 (true
// per-channel cosine) against row 0 and ~0 against the rest, regardless of the other,
// unqueried "appearance" channel's content (segmented query reads only the requested
// range). Also covers GetChannelCount/GetChannelIndex (implemented against the new
// members, but only actually populated once InitWithChannels stores them — so these
// assertions are red today too, on an empty ChannelNames array).
// -----------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSScratchChannelInitQueryTest,
	"SuperFAISS.A.ScratchChannelInitQuery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSScratchChannelInitQueryTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	constexpr int32 Dims = 8;
	const TArray<FName> Names = {TEXT("identity"), TEXT("appearance")};
	const TArray<int32> Offsets = {0, 4};
	const TArray<int32> Lengths = {4, 4};

	USuperFAISSScratchBank* Bank = NewObject<USuperFAISSScratchBank>();
	TestTrue(TEXT("InitWithChannels succeeds"),
		Bank->InitWithChannels(8, Dims, ESuperFAISSBankMetric::Cosine,
			ESuperFAISSBankQuantization::Float32, Names, Offsets, Lengths));
	TestFalse(TEXT("double InitWithChannels rejected"),
		Bank->InitWithChannels(8, Dims, ESuperFAISSBankMetric::Cosine,
			ESuperFAISSBankQuantization::Float32, Names, Offsets, Lengths));

	TestEqual(TEXT("channel count"), Bank->GetChannelCount(), 2);
	TestEqual(TEXT("identity index"), Bank->GetChannelIndex(TEXT("identity")), 0);
	TestEqual(TEXT("appearance index"), Bank->GetChannelIndex(TEXT("appearance")), 1);
	TestEqual(TEXT("unknown channel index"),
		Bank->GetChannelIndex(TEXT("nonexistent")), INDEX_NONE);

	// Orthogonal per-channel basis rows: identity subvec is a one-hot basis vector,
	// appearance subvec the same basis rotated by one — chosen so the expected
	// per-channel cosine against a query is exact and hand-checkable.
	const TArray<float> Row0 = {1, 0, 0, 0, 0, 1, 0, 0};
	const TArray<float> Row1 = {0, 1, 0, 0, 1, 0, 0, 0};
	const TArray<float> Row2 = {0, 0, 1, 0, 0, 0, 1, 0};
	const TArray<float> Row3 = {0, 0, 0, 1, 0, 0, 0, 1};
	int32 Index = INDEX_NONE;
	TestTrue(TEXT("append row0"), Bank->Append(Row0, Index));
	TestEqual(TEXT("row0 index"), Index, 0);
	TestTrue(TEXT("append row1"), Bank->Append(Row1, Index));
	TestTrue(TEXT("append row2"), Bank->Append(Row2, Index));
	TestTrue(TEXT("append row3"), Bank->Append(Row3, Index));
	TestEqual(TEXT("count"), Bank->GetCount(), 4);

	// Query only the "identity" channel, aligned with row 0's identity subvector.
	// The appearance portion of the query is deliberately left zero — a segmented
	// query never reads outside the requested range, so this must not trigger a
	// whole-row zero-norm rejection.
	const TArray<float> Query = {1, 0, 0, 0, 0, 0, 0, 0};
	FSuperFAISSQueryArgs Args;
	Args.K = 4;
	Args.Channels = {{TEXT("identity"), 1.0f}};
	TArray<FSuperFAISSHit> Hits;
	TestTrue(TEXT("named-channel query succeeds"),
		Subsystem->QueryScratch(Bank, Query, Args, Hits));
	TestEqual(TEXT("hit count"), Hits.Num(), 4);
	if (Hits.Num() == 4)
	{
		TestEqual(TEXT("top hit is row 0"), Hits[0].Index, 0);
		TestTrue(TEXT("top hit scores exact cosine 1.0"),
			FMath::IsNearlyEqual(Hits[0].Score, 1.0f, 1e-4f));
		for (int32 i = 1; i < 4; ++i)
		{
			TestTrue(FString::Printf(TEXT("hit %d scores ~0 (orthogonal identity subvec)"), i),
				FMath::IsNearlyEqual(Hits[i].Score, 0.0f, 1e-4f));
		}
	}

	// Unknown channel name is a defined rejection, mirroring the baked path.
	FSuperFAISSQueryArgs Bad;
	Bad.K = 1;
	Bad.Channels = {{TEXT("nonexistent"), 1.0f}};
	TArray<FSuperFAISSHit> None;
	TestFalse(TEXT("unknown channel name rejected"),
		Subsystem->QueryScratch(Bank, Query, Bad, None));

	return true;
}

// -----------------------------------------------------------------------------------
// Cell 2 — named-channel query == the equivalent raw-range Segments query, bit-
// identical (index and score), over both Float32 and Int8 quantization. Anchors that
// a named channel IS exactly its raw range on a scratch bank, the same way
// SuperFAISSChannelTests.cpp anchors it on the baked bank.
// -----------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSScratchChannelEquivalenceTest,
	"SuperFAISS.A.ScratchChannelEquivalence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSScratchChannelEquivalenceTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	constexpr int32 Count = 64;
	constexpr int32 Dims = 32;
	const TArray<FName> Names = {TEXT("identity"), TEXT("appearance")};
	const TArray<int32> Offsets = {0, 16};
	const TArray<int32> Lengths = {16, 16};

	for (ESuperFAISSBankQuantization Quant :
		{ESuperFAISSBankQuantization::Float32, ESuperFAISSBankQuantization::Int8})
	{
		USuperFAISSScratchBank* Bank = NewObject<USuperFAISSScratchBank>();
		TestTrue(TEXT("InitWithChannels succeeds"),
			Bank->InitWithChannels(Count, Dims, ESuperFAISSBankMetric::Cosine, Quant,
				Names, Offsets, Lengths));

		const TArray<float> Rows = ScratchChannelRows(Count, Dims, 0xC4A77E57ull);
		TArray<float> Row;
		Row.SetNumUninitialized(Dims);
		int32 Index = INDEX_NONE;
		for (int32 R = 0; R < Count; ++R)
		{
			FMemory::Memcpy(Row.GetData(), Rows.GetData() + R * Dims, Dims * sizeof(float));
			TestTrue(TEXT("append"), Bank->Append(Row, Index));
		}

		const TArray<float> Query = ScratchChannelRows(1, Dims, 0x9E11ull);

		FSuperFAISSQueryArgs Named;
		Named.K = 8;
		Named.Channels = {{TEXT("identity"), 1.0f}, {TEXT("appearance"), 0.5f}};
		TArray<FSuperFAISSHit> NamedHits;
		TestTrue(TEXT("named query"), Subsystem->QueryScratch(Bank, Query, Named, NamedHits));

		FSuperFAISSQueryArgs Raw;
		Raw.K = 8;
		FSuperFAISSSegment SegA;
		SegA.Offset = 0;
		SegA.Length = 16;
		SegA.Weight = 1.0f;
		FSuperFAISSSegment SegB;
		SegB.Offset = 16;
		SegB.Length = 16;
		SegB.Weight = 0.5f;
		Raw.Segments = {SegA, SegB};
		TArray<FSuperFAISSHit> RawHits;
		TestTrue(TEXT("raw-range query"), Subsystem->QueryScratch(Bank, Query, Raw, RawHits));

		TestEqual(TEXT("named==raw hit count"), NamedHits.Num(), RawHits.Num());
		for (int32 i = 0; i < FMath::Min(NamedHits.Num(), RawHits.Num()); ++i)
		{
			TestTrue(FString::Printf(TEXT("named==raw hit %d"), i),
				NamedHits[i].Index == RawHits[i].Index && NamedHits[i].Score == RawHits[i].Score);
		}
	}

	return true;
}

// -----------------------------------------------------------------------------------
// Cell 3 — FEAT vs an independent brute-force over the channel sub-range (mirrors the
// core TestRelabelFeatOracle shape). The oracle in SubRangeCosine() above is computed
// on the raw pre-append floats, grounded in the definition of cosine similarity —
// never a recode of the scratch bank's own scoring path. Full ranking (K == Count) is
// compared, not just top-1, so a wrong ORDER cannot hide behind a correct top hit.
// -----------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSScratchChannelFeatOracleTest,
	"SuperFAISS.A.ScratchChannelFeatOracle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSScratchChannelFeatOracleTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	constexpr int32 Count = 30;
	constexpr int32 Dims = 16;
	constexpr int32 ChannelOffset = 8;
	constexpr int32 ChannelLength = 8;
	const TArray<FName> Names = {TEXT("left"), TEXT("right")};
	const TArray<int32> Offsets = {0, ChannelOffset};
	const TArray<int32> Lengths = {ChannelOffset, ChannelLength};

	USuperFAISSScratchBank* Bank = NewObject<USuperFAISSScratchBank>();
	TestTrue(TEXT("InitWithChannels succeeds"),
		Bank->InitWithChannels(Count, Dims, ESuperFAISSBankMetric::Cosine,
			ESuperFAISSBankQuantization::Float32, Names, Offsets, Lengths));

	const TArray<float> Rows = ScratchChannelRows(Count, Dims, 0x51DE011Full);
	const TArray<float> Query = ScratchChannelRows(1, Dims, 0xFEA7ull);

	TArray<float> Row;
	Row.SetNumUninitialized(Dims);
	int32 Index = INDEX_NONE;
	for (int32 R = 0; R < Count; ++R)
	{
		FMemory::Memcpy(Row.GetData(), Rows.GetData() + R * Dims, Dims * sizeof(float));
		TestTrue(TEXT("append"), Bank->Append(Row, Index));
	}

	// Independent oracle: brute-force cosine over [ChannelOffset, +ChannelLength) for
	// every row against the query, computed from the raw arrays above — not through
	// the bank, not through any core/plugin scoring call.
	TArray<int32> ExpectedOrder;
	ExpectedOrder.Reserve(Count);
	TArray<double> ExpectedScore;
	ExpectedScore.SetNumUninitialized(Count);
	for (int32 R = 0; R < Count; ++R)
	{
		ExpectedScore[R] = SubRangeCosine(
			Rows.GetData() + R * Dims, Query.GetData(), ChannelOffset, ChannelLength);
		ExpectedOrder.Add(R);
	}
	ExpectedOrder.Sort([&ExpectedScore](int32 A, int32 B) {
		return ExpectedScore[A] > ExpectedScore[B];
	});

	FSuperFAISSQueryArgs Args;
	Args.K = Count;
	Args.Channels = {{TEXT("right"), 1.0f}};
	TArray<FSuperFAISSHit> Hits;
	TestTrue(TEXT("channel query succeeds"), Subsystem->QueryScratch(Bank, Query, Args, Hits));
	TestEqual(TEXT("hit count == row count"), Hits.Num(), Count);
	if (Hits.Num() == Count)
	{
		for (int32 i = 0; i < Count; ++i)
		{
			TestEqual(FString::Printf(TEXT("rank %d matches independent brute force"), i),
				Hits[i].Index, ExpectedOrder[i]);
			TestTrue(FString::Printf(TEXT("rank %d score matches independent brute force"), i),
				FMath::IsNearlyEqual(Hits[i].Score,
					static_cast<float>(ExpectedScore[ExpectedOrder[i]]), 1e-3f));
		}
	}

	return true;
}

// -----------------------------------------------------------------------------------
// Cell 4 — malformed channel table rejected at InitWithChannels: overlap, off-grid,
// duplicate names, mismatched Names/Offsets/Lengths array lengths. Mirrors the baked
// bank's rejection matrix (SuperFAISSChannelTests.cpp). Against today's stub these
// all trivially return false (the stub returns false unconditionally) — they are
// authored for the green contract the kickoff states, not for red-today signal; a
// green implementation must reject each for ITS OWN reason, not merely inherit a
// blanket false.
// -----------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSScratchChannelMalformedTableTest,
	"SuperFAISS.A.ScratchChannelMalformedTable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSScratchChannelMalformedTableTest::RunTest(const FString& Parameters)
{
	constexpr int32 Dims = 32;

	// Overlap: "a" spans [0,16), "b" spans [8,16) — overlapping ranges.
	{
		USuperFAISSScratchBank* Bank = NewObject<USuperFAISSScratchBank>();
		TestFalse(TEXT("overlap rejected"),
			Bank->InitWithChannels(16, Dims, ESuperFAISSBankMetric::Cosine,
				ESuperFAISSBankQuantization::Float32,
				{TEXT("a"), TEXT("b")}, {0, 8}, {16, 8}));
	}

	// Off-grid: offset 2 is not on the Float32 16-byte element grid (4 elements).
	{
		USuperFAISSScratchBank* Bank = NewObject<USuperFAISSScratchBank>();
		TestFalse(TEXT("off-grid offset rejected"),
			Bank->InitWithChannels(16, Dims, ESuperFAISSBankMetric::Cosine,
				ESuperFAISSBankQuantization::Float32,
				{TEXT("a")}, {2}, {8}));
	}

	// Duplicate names: "a" appears twice.
	{
		USuperFAISSScratchBank* Bank = NewObject<USuperFAISSScratchBank>();
		TestFalse(TEXT("duplicate names rejected"),
			Bank->InitWithChannels(16, Dims, ESuperFAISSBankMetric::Cosine,
				ESuperFAISSBankQuantization::Float32,
				{TEXT("a"), TEXT("a")}, {0, 16}, {16, 16}));
	}

	// Mismatched array lengths: two names, only one offset.
	{
		USuperFAISSScratchBank* Bank = NewObject<USuperFAISSScratchBank>();
		TestFalse(TEXT("mismatched Names/Offsets/Lengths rejected"),
			Bank->InitWithChannels(16, Dims, ESuperFAISSBankMetric::Cosine,
				ESuperFAISSBankQuantization::Float32,
				{TEXT("a"), TEXT("b")}, {0}, {16, 16}));
	}

	// A malformed InitWithChannels must leave the bank uninitialized (never a
	// half-constructed bank a caller could mistake for a live one) — reject-over-
	// degrade, the same posture Init and LoadFromBytes hold elsewhere on this class.
	{
		USuperFAISSScratchBank* Bank = NewObject<USuperFAISSScratchBank>();
		TestFalse(TEXT("overlap rejected (for the IsInitialized check below)"),
			Bank->InitWithChannels(16, Dims, ESuperFAISSBankMetric::Cosine,
				ESuperFAISSBankQuantization::Float32,
				{TEXT("a"), TEXT("b")}, {0, 8}, {16, 8}));
		TestFalse(TEXT("bank left uninitialized after a rejected table"),
			Bank->IsInitialized());
	}

	return true;
}

// -----------------------------------------------------------------------------------
// Cell 5 — P-1 renorm bounds safety (committed guard 16830d4). On a Cosine
// channel scratch bank the per-channel-cosine renorm reads/writes the padded query
// staging over each segment's range BEFORE the core Query() validates segment bounds.
// A raw Args.Segments range with offset+length > paddedDims used to over-read/over-write
// that staging buffer first (a heap over-read — UB, may or may not crash). The guard
// skips out-of-range segments in the renorm; the core then rejects the malformed query
// as before. The renorm branch is active only on a Cosine bank with GetChannelCount()>0,
// so this test builds exactly that and drives a RAW (not named-channel) out-of-bounds
// segment — named-channel segments always resolve in range and never reach the guard.
//
// Oracle: the out-of-bounds raw-segment query returns FALSE (the core's defined
// rejection), the process does not crash or corrupt, and a normal in-bounds query on
// the SAME bank still succeeds afterward (the staging buffer was not left dirty). This
// asserts the defined post-guard behavior; pre-guard the over-read was UB, so its value
// is locking the rejection, not reproducing an unstable crash.
// -----------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSScratchChannelSegmentBoundsTest,
	"SuperFAISS.A.ScratchChannelSegmentBounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSScratchChannelSegmentBoundsTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	constexpr int32 Count = 16;
	constexpr int32 Dims = 8;   // Float32 paddedDims == 8; any segment past 8 is OOB
	const TArray<FName> Names = {TEXT("identity"), TEXT("appearance")};
	const TArray<int32> Offsets = {0, 4};
	const TArray<int32> Lengths = {4, 4};

	USuperFAISSScratchBank* Bank = NewObject<USuperFAISSScratchBank>();
	TestTrue(TEXT("InitWithChannels (Cosine, channels -> renorm path active)"),
		Bank->InitWithChannels(Count, Dims, ESuperFAISSBankMetric::Cosine,
			ESuperFAISSBankQuantization::Float32, Names, Offsets, Lengths));
	TestTrue(TEXT("renorm precondition: bank carries channels"),
		Bank->GetChannelCount() > 0);

	const TArray<float> Rows = ScratchChannelRows(Count, Dims, 0x9A11B0D5ull);
	TArray<float> Row;
	Row.SetNumUninitialized(Dims);
	int32 Index = INDEX_NONE;
	for (int32 R = 0; R < Count; ++R)
	{
		FMemory::Memcpy(Row.GetData(), Rows.GetData() + R * Dims, Dims * sizeof(float));
		TestTrue(TEXT("append"), Bank->Append(Row, Index));
	}

	const TArray<float> Query = ScratchChannelRows(1, Dims, 0x5E60B0D5ull);

	// Two out-of-bounds RAW segments (offset+length > paddedDims==8). Each drives the
	// guarded renorm loop, then the core rejects the malformed range -> QueryScratch
	// false. Reaching here without a crash IS the over-read guard holding.
	{
		FSuperFAISSQueryArgs Args;
		Args.K = 4;
		FSuperFAISSSegment Seg;
		Seg.Offset = 0;
		Seg.Length = 12;   // 0 + 12 > 8
		Seg.Weight = 1.0f;
		Args.Segments = {Seg};
		TArray<FSuperFAISSHit> Hits;
		TestFalse(TEXT("OOB segment (offset 0, length 12) rejected"),
			Subsystem->QueryScratch(Bank, Query, Args, Hits));
		TestEqual(TEXT("no hits on rejection"), Hits.Num(), 0);
	}
	{
		FSuperFAISSQueryArgs Args;
		Args.K = 4;
		FSuperFAISSSegment Seg;
		Seg.Offset = 4;
		Seg.Length = 8;    // 4 + 8 > 8
		Seg.Weight = 1.0f;
		Args.Segments = {Seg};
		TArray<FSuperFAISSHit> Hits;
		TestFalse(TEXT("OOB segment (offset 4, length 8) rejected"),
			Subsystem->QueryScratch(Bank, Query, Args, Hits));
	}

	// The staging buffer was not left dirty: a normal in-bounds named-channel query on
	// the SAME bank still succeeds and returns the full set of live rows.
	{
		FSuperFAISSQueryArgs Args;
		Args.K = Count;
		Args.Channels = {{TEXT("identity"), 1.0f}};
		TArray<FSuperFAISSHit> Hits;
		TestTrue(TEXT("in-bounds named-channel query still succeeds after OOB rejections"),
			Subsystem->QueryScratch(Bank, Query, Args, Hits));
		TestEqual(TEXT("in-bounds query returns all live rows"), Hits.Num(), Count);
	}

	// And an in-bounds RAW segment (offset 0, length 4 — a valid sub-range) succeeds too,
	// confirming the guard rejects only the out-of-range case, not raw segments per se.
	{
		FSuperFAISSQueryArgs Args;
		Args.K = Count;
		FSuperFAISSSegment Seg;
		Seg.Offset = 0;
		Seg.Length = 4;
		Seg.Weight = 1.0f;
		Args.Segments = {Seg};
		TArray<FSuperFAISSHit> Hits;
		TestTrue(TEXT("in-bounds raw segment (offset 0, length 4) succeeds"),
			Subsystem->QueryScratch(Bank, Query, Args, Hits));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
