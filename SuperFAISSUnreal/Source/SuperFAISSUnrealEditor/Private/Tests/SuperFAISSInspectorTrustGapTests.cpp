// S-INSP-3.3.1 Gate 1b: the red suite for SF34-002 (archive file-picker control flow) and
// SF34-007 (claims-vs-code reconciliation), realizing the plan's Coverage Model
// (Claude/Plans/SuperFAISSUnreal_3.3.1_Plan.md section 6) dims 2, 5, 7, 11c. These tickets are
// NOT oracle-gated (section 3's oracle-gated set is {SF34-003,004,005,006}) -- their
// acceptance is control-flow and guard-vitality, not a geometry-derived numeric answer, so no
// tutorial-bank fixture is needed here.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "SSuperFAISSBankInspector.h"
#include "SuperFAISSScratchBank.h"
#include "SuperFAISSVectorBank.h"
#include "superfaiss/scratch.h"

namespace
{
	TArray<float> SeededRows(int32 Count, int32 Dims, uint64 Seed)
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

	bool BuildArchiveBytes(FAutomationTestBase& Test, int32 Count, int32 Dims, uint64 Seed,
		TArray<uint8>& OutBytes)
	{
		USuperFAISSScratchBank* Scratch = NewObject<USuperFAISSScratchBank>();
		if (!Scratch->Init(Count, Dims, ESuperFAISSBankMetric::L2, ESuperFAISSBankQuantization::Float32))
		{
			Test.AddError(TEXT("archive fixture: Init failed"));
			return false;
		}
		const TArray<float> Rows = SeededRows(Count, Dims, Seed);
		for (int32 i = 0; i < Count; ++i)
		{
			TArray<float> Row;
			Row.Append(&Rows[static_cast<int64>(i) * Dims], Dims);
			int32 OutIndex = INDEX_NONE;
			if (!Scratch->Append(Row, OutIndex))
			{
				Test.AddError(FString::Printf(TEXT("archive fixture: Append row %d failed"), i));
				return false;
			}
		}
		return Scratch->SaveToBytes(OutBytes);
	}
}

// ===========================================================================
// SF34-002 (Coverage Model dim 2/5) -- the "Open scratch archive..." control flow on BOTH
// slots: open / replace / reject-on-bad-bytes / preserve-current-source-on-a-failed-open.
// The handler primitives themselves (OpenScratchArchiveFromBytes/
// OpenSecondScratchArchiveFromBytes) are already real per the roadmap grounding (T-01's own
// characterization: "byte handlers exist, no visible affordance") -- these cells pin the
// CONTROL-FLOW CONTRACT the plan's acceptance criteria state explicitly ("Both slots
// open/replace/reject/close from visible UI; a failed open preserves the current source"),
// as a standing regression proof that survives the UI affordance landing on top of it, not as
// a claim that the handlers are new achievement.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSArchiveOpenReplaceRejectPreserveTest,
	"SuperFAISS.D.ArchiveOpenReplaceRejectPreserve",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSArchiveOpenReplaceRejectPreserveTest::RunTest(const FString& Parameters)
{
	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);

	// Open: a valid archive becomes the primary source.
	TArray<uint8> ArchiveA;
	TestTrue(TEXT("(setup) archive A bakes"), BuildArchiveBytes(*this, 10, 4, 0xA001, ArchiveA));
	TestTrue(TEXT("open: a valid archive opens"), Inspector->OpenScratchArchiveFromBytes(ArchiveA, TEXT("A.bin")));
	TestEqual(TEXT("open: primary source is Archive-kind"),
		static_cast<int32>(Inspector->GetPrimarySource().Kind),
		static_cast<int32>(FSuperFAISSInspectionSource::EKind::Archive));
	TestEqual(TEXT("open: primary source live count matches the opened archive"),
		Inspector->GetPrimarySource().GetLiveCount(), 10);

	// Replace: opening a second valid archive on the SAME slot supersedes the first.
	TArray<uint8> ArchiveB;
	TestTrue(TEXT("(setup) archive B bakes"), BuildArchiveBytes(*this, 6, 4, 0xB002, ArchiveB));
	TestTrue(TEXT("replace: a second valid archive opens on the same slot"),
		Inspector->OpenScratchArchiveFromBytes(ArchiveB, TEXT("B.bin")));
	TestEqual(TEXT("replace: primary source now reflects archive B, not A"),
		Inspector->GetPrimarySource().GetLiveCount(), 6);

	// Reject + preserve: a malformed buffer fails to open and leaves the CURRENT source
	// (archive B, from the replace above) exactly as it was.
	TArray<uint8> Malformed = {1, 2, 3, 4, 5, 6, 7, 8};
	TestFalse(TEXT("reject: a malformed buffer fails to open"),
		Inspector->OpenScratchArchiveFromBytes(Malformed, TEXT("bad.bin")));
	TestTrue(TEXT("reject: a specific rejection status is set"), !Inspector->GetArchiveOpenStatus().IsEmpty());
	TestEqual(TEXT("preserve: the current source (archive B) survives a failed open unchanged"),
		Inspector->GetPrimarySource().GetLiveCount(), 6);
	TestEqual(TEXT("preserve: primary source is still Archive-kind after the failed open"),
		static_cast<int32>(Inspector->GetPrimarySource().Kind),
		static_cast<int32>(FSuperFAISSInspectionSource::EKind::Archive));

	// The second-bank slot mirrors the same contract, independently.
	TArray<uint8> ArchiveC;
	TestTrue(TEXT("(setup) archive C bakes"), BuildArchiveBytes(*this, 8, 4, 0xC003, ArchiveC));
	TestTrue(TEXT("second slot: a valid archive opens"),
		Inspector->OpenSecondScratchArchiveFromBytes(ArchiveC, TEXT("C.bin")));
	TestEqual(TEXT("second slot: second source is Archive-kind"),
		static_cast<int32>(Inspector->GetSecondSource().Kind),
		static_cast<int32>(FSuperFAISSInspectionSource::EKind::Archive));
	TestFalse(TEXT("second slot: a malformed buffer fails to open"),
		Inspector->OpenSecondScratchArchiveFromBytes(Malformed, TEXT("bad2.bin")));
	TestEqual(TEXT("second slot: the current source (archive C) survives a failed open unchanged"),
		Inspector->GetSecondSource().GetLiveCount(), 8);

	return true;
}

// ===========================================================================
// Guard vitality (dim 11c): PeekScratchArchive's validation paths must actually TRIP on
// truncated, trailing-data, and malformed bytes with the specific failure this
// release's T-11/SF34-007 surfaces depend on -- proven at the vendored core library boundary
// directly (superfaiss::PeekScratchArchive is already shipped per the roadmap grounding G-5;
// this cell proves ITS OWN contract fires correctly, independent of whether any editor caller
// exists yet, which is SF34-002/007's own gap, covered separately above and in the
// claims-vs-code matrix below).
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSPeekScratchArchiveGuardVitalityTest,
	"SuperFAISS.D.PeekScratchArchiveGuardVitality",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSPeekScratchArchiveGuardVitalityTest::RunTest(const FString& Parameters)
{
	using namespace superfaiss;

	// Positive control: a real, valid archive peeks Ok and reports the correct geometry --
	// proves the guard below is discriminating (fires on bad input, not on everything).
	TArray<uint8> Valid;
	TestTrue(TEXT("(setup) a valid archive bakes"), BuildArchiveBytes(*this, 12, 4, 0xD004, Valid));
	{
		ScratchArchiveInfo Info;
		const Status PeekStatus = PeekScratchArchive(Valid.GetData(), Valid.Num(), &Info);
		TestEqual(TEXT("positive control: a valid archive peeks Ok"),
			static_cast<int>(PeekStatus), static_cast<int>(Status::Ok));
		TestEqual(TEXT("positive control: peeked count matches the baked archive"), Info.count, 12);
		TestEqual(TEXT("positive control: peeked dims match the baked archive"), Info.dims, 4);
		TestTrue(TEXT("positive control: archiveBytes is positive and does not exceed the buffer"),
			Info.archiveBytes > 0 && Info.archiveBytes <= Valid.Num());
	}

	// Truncated: a prefix of a valid archive (shorter than the header declares) is BadFormat,
	// never a crash and never a false Ok.
	{
		TArray<uint8> Truncated = Valid;
		Truncated.SetNum(FMath::Max(1, Valid.Num() / 4));
		ScratchArchiveInfo Info;
		const Status PeekStatus = PeekScratchArchive(Truncated.GetData(), Truncated.Num(), &Info);
		TestNotEqual(TEXT("dim 11c: a truncated archive is rejected, not read as Ok"),
			static_cast<int>(PeekStatus), static_cast<int>(Status::Ok));
	}

	// Trailing data: a valid archive with extra bytes appended peeks Ok and reports
	// archiveBytes STRICTLY LESS than the total buffer length -- the exact contract T-11's
	// trailing-data status line and SF34-002's peek-gated open depend on (locating the
	// trailer boundary before anything is committed).
	{
		TArray<uint8> WithTrailer = Valid;
		WithTrailer.Append({0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11, 0x22, 0x33});
		ScratchArchiveInfo Info;
		const Status PeekStatus = PeekScratchArchive(WithTrailer.GetData(), WithTrailer.Num(), &Info);
		TestEqual(TEXT("dim 11c: an archive with trailing bytes still peeks Ok"),
			static_cast<int>(PeekStatus), static_cast<int>(Status::Ok));
		TestTrue(TEXT("dim 11c: archiveBytes locates the trailer boundary (strictly less than the full buffer)"),
			Info.archiveBytes > 0 && Info.archiveBytes < WithTrailer.Num());
	}

	// Malformed: a fixed non-archive byte pattern, the same length as a real archive, is
	// BadFormat.
	{
		TArray<uint8> Malformed;
		Malformed.SetNumUninitialized(Valid.Num());
		FMemory::Memset(Malformed.GetData(), 0x5A, Malformed.Num());
		ScratchArchiveInfo Info;
		const Status PeekStatus = PeekScratchArchive(Malformed.GetData(), Malformed.Num(), &Info);
		TestNotEqual(TEXT("dim 11c: malformed (non-archive) bytes are rejected, not read as Ok"),
			static_cast<int>(PeekStatus), static_cast<int>(Status::Ok));
	}

	return true;
}

// ===========================================================================
// SF34-007 (Coverage Model dim 7) -- the claims-vs-code capability matrix. A minimal,
// data-driven proof that the specific capability claims this release makes (or corrects) are
// each backed by a reachable code path, exercised here rather than merely read. Two of
// T-07/T-10/T-11's three NEW DISPLAY SURFACES (the archive metadata header line, the complete
// tooltip + doc URL) are NOT included below -- see the test-design artifact's routed
// Coverage-Model gap: the plan states their INTENT but not the exact rendered string/format a
// red assertion needs, and inventing one would couple the suite to Curie's own guess rather
// than the design's. T-11 (peek geometry + trailing-data status before load) is covered here
// at the level currently assertable: the underlying PeekScratchArchive contract (proven above)
// plus the archive open/reject/preserve contract (proven above) are the two facts the display
// surface will report; the display STRING itself is the same routed gap as T-07/T-10.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSClaimsVsCodeCapabilityMatrixTest,
	"SuperFAISS.D.ClaimsVsCodeCapabilityMatrix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSClaimsVsCodeCapabilityMatrixTest::RunTest(const FString& Parameters)
{
	// Capability: "Open scratch archive..." exists on both the primary AND second-bank
	// slots (the 3.2.0 changelog's own claim, G-13 -- true today at the handler level, this
	// matrix cell keeps it proven as the UI affordance lands on top).
	{
		TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
		TArray<uint8> Bytes;
		TestTrue(TEXT("(setup) capability-matrix archive bakes"), BuildArchiveBytes(*this, 5, 4, 0xF006, Bytes));
		TestTrue(TEXT("capability: primary slot can open a scratch archive"),
			Inspector->OpenScratchArchiveFromBytes(Bytes, TEXT("cap.bin")));
		TestTrue(TEXT("capability: second slot can open a scratch archive independently"),
			Inspector->OpenSecondScratchArchiveFromBytes(Bytes, TEXT("cap2.bin")));
	}

	// Capability: Novelty/Query analysis works on either source kind (SF34-003's own
	// acceptance) -- the tutorial-bank oracle test file proves the ANSWER is correct; this
	// cell is the narrower "the capability is reachable at all" claim, kept here so the
	// matrix is self-contained without re-deriving geometry.
	{
		TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
		TArray<uint8> Bytes;
		// 9 rows, not 8: ProbeNovelty's limb 2 rejects "sample too small for the configured
		// NoveltyK" whenever SampledView.count <= NoveltyK, and the default NoveltyK is 8 --
		// an 8-row archive sits exactly on that boundary and never reaches a valid verdict,
		// regardless of source kind. One extra row clears the guard without changing what
		// this cell is actually about (reachability, not a specific verdict).
		TestTrue(TEXT("(setup) capability-matrix archive bakes"), BuildArchiveBytes(*this, 9, 4, 0xF007, Bytes));
		TestTrue(TEXT("capability: archive opens"), Inspector->OpenScratchArchiveFromBytes(Bytes, TEXT("nov.bin")));
		Inspector->ProbeNovelty(TEXT("#0"));
		TestTrue(TEXT("capability (SF34-003): Novelty is reachable on an archive source "
			"(claim: \"complete UI access\" implies not asset-only)"),
			Inspector->GetNoveltyResult().bValid);
	}

	// Capability: channel-scoped analysis works on an archive source (SF34-005's own
	// acceptance) -- reachability only; the tutorial-bank oracle test file proves the answer.
	{
		const TArray<FName> ChannelNames = {TEXT("chanA"), TEXT("chanB")};
		const TArray<int32> ChannelOffsets = {0, 4};
		const TArray<int32> ChannelLengths = {4, 4};

		// A channel-carrying ASSET first: the projection-scope combo (ProjectionScopes /
		// SetAnalysisScopeForTest's match set) is asset-driven regardless of which source
		// ends up primary (section 25.3's design note) -- opening the archive alone, with
		// no asset ever selected, leaves ProjectionScopes at only "(whole row)", so
		// SetAnalysisScopeForTest("chanA") below would silently no-op and this cell would
		// vacuously pass at whole-row scope without ever reaching the chanA path it claims
		// to prove reachable.
		USuperFAISSVectorBank* ChannelAsset = NewObject<USuperFAISSVectorBank>();
		FString AssetError;
		TestTrue(TEXT("(setup) channel-carrying asset builds"),
			ChannelAsset->InitFromSource(SeededRows(4, 8, 0xF009), 4, 8, ESuperFAISSBankMetric::Cosine,
				ESuperFAISSBankQuantization::Float32, {}, TEXT("capability-matrix-test"), AssetError,
				ChannelNames, ChannelOffsets, ChannelLengths));

		USuperFAISSScratchBank* Scratch = NewObject<USuperFAISSScratchBank>();
		TestTrue(TEXT("(setup) channel-carrying archive inits"),
			Scratch->InitWithChannels(4, 8, ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Float32,
				ChannelNames, ChannelOffsets, ChannelLengths));
		const TArray<float> Rows = SeededRows(4, 8, 0xF008);
		for (int32 i = 0; i < 4; ++i)
		{
			TArray<float> Row;
			Row.Append(&Rows[static_cast<int64>(i) * 8], 8);
			int32 OutIndex = INDEX_NONE;
			TestTrue(TEXT("(setup) channel-archive row appends"), Scratch->Append(Row, OutIndex));
		}
		TArray<uint8> Bytes;
		TestTrue(TEXT("(setup) channel archive saves"), Scratch->SaveToBytes(Bytes));

		TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
		Inspector->SetBankForTest(ChannelAsset); // populates the scope combo
		Inspector->SetAnalysisScopeForTest(TEXT("chanA"));
		TestTrue(TEXT("(setup) channel archive opens"), Inspector->OpenScratchArchiveFromBytes(Bytes, TEXT("chan.bin")));
		const FSuperFAISSInspectionSource Source = Inspector->GetPrimarySource();
		TArray<uint8, TAlignedHeapAllocator<16>> Payload;
		TArray<float> Scales;
		superfaiss::BankView View;
		TArray<int32> SourceIndices;
		const bool bBuilt = Inspector->BuildAnalysisSampleForTest(Source, Source.GetCount(), Payload, Scales,
			View, SourceIndices);
		TestTrue(TEXT("capability (SF34-005): channel-scoped analysis is reachable on an archive source"), bBuilt);
		if (bBuilt)
		{
			TestEqual(TEXT("capability (SF34-005): the reachable path is genuinely chanA-scoped "
				"(dims == 4), not a silent whole-row fallback"), View.dims, 4);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
