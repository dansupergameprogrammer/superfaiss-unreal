// S-INSP-3.3.1 Gate-2 gap closure: the red suite for the cells named by a coverage audit and by
// finding 4 of a code review of the 3.3.1 release, realizing the six cells this round's brief
// assigns for direct authoring:
//   - T-06 (the HEAVY-pass disclosure), RETARGETED under T-815 -- see the header comment on
//     CorrespondenceHeavyPassDisclosure below -- the cell as first authored asserted a post-run
//     status suffix that two pinned pre-existing cells
//     (SuperFAISSInspectorPanelTests.cpp:2761, :2850) already assert does NOT carry it; the
//     CHANGELOG's own claim names a pre-run disclosure. The seam it needed,
//     GetPendingCorrespondenceDisclosure(), now exists in the shipped tree (built under T-815)
//     and the cell below asserts against it directly -- CorrespondenceHeavyPassDisclosure
//   - T-11's uncalled public accessor (GetArchivePeekGeometry/GetSecondArchivePeekGeometry) --
//     ArchivePeekGeometryDisclosure
//   - the Novelty-on-tombstoned-archive composition cell the audit's C2 finding names --
//     TutorialArchiveNoveltyTombstoneComposition
//   - the second slot's untested archive legs (channel-scope, correspondence) --
//     TutorialSecondSlotArchiveChannelScopeParity, TutorialSecondSlotArchiveCorrespondenceParity
//   - T-10's tooltip half (the doc-URL half has no existing surface at all and is routed
//     separately in the test-design record) -- CslsMarginThresholdTooltipCompleteness
//
// Every test below compiles and runs against the CURRENT shipped tree -- none references a
// symbol that does not exist yet (unlike the seams routed in the design record's own later
// sections, which are documented there rather than committed as non-compiling source, matching
// this project's own established precedent: the T-03 gap was originally routed in prose in the
// 2026-07-22 test-design before GetStructureMemberLabelForTest existed to write against).
//
// A round-2 closeout review adds one new cell and extends one existing cell below, all in this
// file:
//   - S-1/S-2's structural fix -- the README test-count claim checked at runtime rather than
//     maintained by hand -- ReadmeTestCountAssertion (new)
//   - S-4 -- the doc-URL cell's assertion (b) guarded only the section's prose, not the
//     heading-to-anchor link the plan names as the one way this link dies -- extended with a
//     GitHub slug-derivation assertion, in CorrespondenceDocumentationUrl's own RunTest, (c)
//   - M-1 -- a stale comment calling this plugin's own README.md "the same file the URL
//     targets" -- corrected in CorrespondenceDocumentationUrl's assertion (b) comment
//   - M-2 -- plan sec10.2's hyperlink-reachability assertion, never authored -- added as
//     assertion (d) in CorrespondenceDocumentationUrl's own RunTest
//
// Board T-1100 (2026-07-26, an outside code review's finding 1; re-filed from a colliding
// T-1002 -- see board row T-1101): opening a SECOND channel-carrying primary archive leaves the
// FIRST archive's channel slider labels on screen while RunQuery binds those sliders' weights
// positionally to the NEW archive's channel names -- ArchiveOpenChannelSliderLabelIdentity,
// added below, closed by the primary-archive-resync fix in SSuperFAISSBankInspector.cpp's
// OpenScratchArchiveFromBytes(), via its unconditional call to ResyncChannelSlidersToPrimarySource().
//
// Board T-1121 (2026-07-26, a confirmation review of the T-1100 remedy, finding 2): the same
// mislabelled-weight failure was found in the ASSET-to-archive form -- the guard's
// asset-driven-by-design exclusion meant the FIRST primary-archive open after an asset selection
// was never resynced, so the asset's stale channel labels stayed on screen while RunQuery bound
// their weights to the archive's channels by position -- AssetThenArchiveChannelSliderLabel
// Identity, added below, pins the SAME invariant ArchiveOpenChannelSliderLabelIdentity pins, for
// the sequence that cell cannot see. CLOSED by D-INSP-27 below: ResyncChannelSlidersToPrimarySource()
// now runs unconditionally on every primary-source change, independent of the projection-scope
// guard (T-1123's seam unblocked the fixture, not the fix), so the label/weight binding this
// cell pins is correct on the asset-to-archive sequence too -- AssetThenArchiveChannelSliderLabel
// Identity is green.
//
// D-INSP-27 (2026-07-26): channel weights reset to their default on EVERY primary-source
// change, asset or archive; the by-name carryover is removed. The seam named above (T-1123) is
// built, and it unblocked the two cells below: PrimaryBankChangeResetsChannelWeights (V32-G2's
// reset claim on the asset->asset path) and ArchiveResyncResetsChannelWeights (the same claim on
// the exact archive-resync path T-1100's carryover was written for). Both were authored red
// against the carryover, then went green when ResyncChannelSlidersToPrimarySource() started
// writing 1.0f unconditionally.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/SuperFAISSTutorialBankFixture.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Internationalization/Regex.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SSuperFAISSBankInspector.h"
#include "SuperFAISSInspectorSettings.h"
#include "UObject/UnrealType.h"
#include "Widgets/Text/STextBlock.h"
#include "superfaiss/superfaiss.h"

using namespace SuperFAISSTutorialBank;

namespace
{
	// Mirrors SuperFAISSTutorialBankOracleTests.cpp's own FSettingsGuard exactly (same
	// snapshot/restore discipline -- a test that pins MatchK/CslsMarginThreshold/SampleLimit
	// for its own derivation never leaks into a later test in the same process).
	struct FSettingsGuard
	{
		USuperFAISSInspectorSettings* Settings = GetMutableDefault<USuperFAISSInspectorSettings>();
		float OrigThreshold = Settings->CslsMarginThreshold;
		int32 OrigMatchK = Settings->MatchK;
		int32 OrigSampleLimit = Settings->SampleLimit;
		~FSettingsGuard()
		{
			Settings->CslsMarginThreshold = OrigThreshold;
			Settings->MatchK = OrigMatchK;
			Settings->SampleLimit = OrigSampleLimit;
		}
	};

	// Mirrors SuperFAISSTutorialBankOracleTests.cpp's own OpenTutorialArchive helper exactly
	// (duplicated rather than shared across the two .cpp files -- this codebase's existing
	// convention keeps each test file's local helpers local; see e.g.
	// SuperFAISSInspectorTrustGapTests.cpp's own BuildArchiveBytes, which is likewise not
	// shared with the oracle file).
	bool OpenTutorialArchive(FAutomationTestBase& Test, SSuperFAISSBankInspector& Inspector,
		const FString& BankName, const TArray<int32>& RowsToTombstone, bool bSecondSlot)
	{
		TArray<uint8> Bytes;
		if (!BakeAsArchiveBytes(Test, BankName, RowsToTombstone, Bytes))
		{
			return false;
		}
		const bool bOpened = bSecondSlot
			? Inspector.OpenSecondScratchArchiveFromBytes(Bytes, BankName + TEXT("-archive"))
			: Inspector.OpenScratchArchiveFromBytes(Bytes, BankName + TEXT("-archive"));
		Test.TestTrue(FString::Printf(TEXT("tutorial bank '%s' archive opens"), *BankName), bOpened);
		return bOpened;
	}

	// Round-2 closeout (2026-07-26, outside-review findings P1/P2): a deterministic row
	// generator local to the cells below, independent of ScratchRows/ChannelRows/
	// ScratchChannelRows in sibling files, per this file's own established convention of
	// file-local fixture helpers (see OpenTutorialArchive's own header comment above).
	TArray<float> GapClosureRows(int32 Count, int32 Dims, uint64 Seed)
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

	// A channel-carrying (or, with empty arrays, zero-channel) ASSET built via InitFromSource --
	// the D-group test convention (SuperFAISSInspectorPanelTests.cpp's own MakeBank),
	// reproduced here per this file's per-file-local-helper convention. The tutorial fixture's
	// own BakeAsAsset always uses the fixed chanA/chanB geometry, which cannot express the
	// DIFFERENT-channel or ZERO-channel asset shapes P1's round-2 cells need.
	USuperFAISSVectorBank* MakeGapClosureAsset(FAutomationTestBase& Test, int32 Count, int32 Dims,
		TConstArrayView<FName> ChannelNames = {}, TConstArrayView<int32> ChannelOffsets = {},
		TConstArrayView<int32> ChannelLengths = {}, uint64 Seed = 0xA55E7ull)
	{
		USuperFAISSVectorBank* Bank = NewObject<USuperFAISSVectorBank>();
		FString Error;
		const bool bOk = Bank->InitFromSource(GapClosureRows(Count, Dims, Seed), Count, Dims,
			ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Float32, {},
			TEXT("gap-closure-round2-test"), Error, ChannelNames, ChannelOffsets, ChannelLengths);
		Test.TestTrue(FString::Printf(TEXT("(setup) asset built: %s"), *Error), bOk);
		return bOk ? Bank : nullptr;
	}

	// A channel-carrying scratch archive's SAVED BYTES, in-memory (mirrors
	// SuperFAISSInspectorPanelTests.cpp's own MakeScratchArchiveBytes, channel-carrying
	// variant, reproduced locally per the same convention).
	bool MakeGapClosureArchiveBytes(FAutomationTestBase& Test, int32 Count, int32 Dims,
		const TArray<FName>& ChannelNames, const TArray<int32>& ChannelOffsets,
		const TArray<int32>& ChannelLengths, uint64 Seed, TArray<uint8>& OutBytes)
	{
		USuperFAISSScratchBank* Scratch = NewObject<USuperFAISSScratchBank>();
		if (!Scratch->InitWithChannels(Count, Dims, ESuperFAISSBankMetric::Cosine,
			ESuperFAISSBankQuantization::Float32, ChannelNames, ChannelOffsets, ChannelLengths))
		{
			Test.AddError(TEXT("(setup) channel archive fixture: InitWithChannels failed"));
			return false;
		}
		const TArray<float> Rows = GapClosureRows(Count, Dims, Seed);
		TArray<float> Row;
		Row.SetNumUninitialized(Dims);
		for (int32 i = 0; i < Count; ++i)
		{
			FMemory::Memcpy(Row.GetData(), Rows.GetData() + static_cast<int64>(i) * Dims,
				Dims * sizeof(float));
			int32 OutIndex = INDEX_NONE;
			if (!Scratch->Append(Row, OutIndex))
			{
				Test.AddError(TEXT("(setup) channel archive fixture: row append failed"));
				return false;
			}
		}
		return Scratch->SaveToBytes(OutBytes);
	}
}

// ===========================================================================
// T-06 -- the HEAVY-pass disclosure, RETARGETED (T-815, 2026-07-25). The cell as originally
// authored asserted GetCorrespondenceStatus().Contains("HEAVY") on a COMPLETED run's status.
// That can never pass: two pre-existing, PINNED cells --
// SuperFAISSInspectorPanelTests.cpp:2761 (InspectorCorrespondenceLiveCountDenominators) and
// :2850 (InspectorCorrespondenceZeroEnergyDenominators) -- already assert that exact status
// string with TestEqual and no HEAVY clause, on fixtures that equally reach the matching
// kernel (MutualNearestMatches). No string can both contain a token and equal one that omits
// it, so the original cell demanded weakening one of those two pinned assertions to ever go
// green -- out of scope here and not this seat's call regardless (SuperFAISSInspectorPanelTests.cpp
// is read-only for this round).
//
// The resolution is in the claim's own wording. CHANGELOG.md's [3.2.0] entry (View C): "Disclosed
// as the HEAVY pass in the set -- cost scales with both banks' sizes -- BEFORE IT RUNS." That is
// a PRE-RUN cost disclosure, not a post-run status suffix -- the post-run CorrespondenceStatus
// was never the surface the claim describes, and the two pinned cells are correct as they stand.
//
// The seam this claim needed, GetPendingCorrespondenceDisclosure(), is now built (T-815,
// 2026-07-25) -- declared in SSuperFAISSBankInspector.h, implemented in
// SSuperFAISSBankInspector.cpp: read-only, callable before ComputeCorrespondence() runs,
// empty when either slot resolves to EKind::None, otherwise names the HEAVY pass and carries
// both banks' live counts. The cell below realizes the full claim against that seam directly:
// two negative controls (neither slot resolved; only the primary slot resolved) and one
// positive control asserting the exact disclosure string, read strictly before this test ever
// calls ComputeCorrespondence().
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSCorrespondenceHeavyPassDisclosureTest,
	"SuperFAISS.D.CorrespondenceHeavyPassDisclosure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSCorrespondenceHeavyPassDisclosureTest::RunTest(const FString& Parameters)
{
	USuperFAISSVectorBank* Primary = BakeAsAsset(*this, TEXT("Primary"));
	USuperFAISSVectorBank* Secondary = BakeAsAsset(*this, TEXT("Secondary"));
	if (Primary == nullptr || Secondary == nullptr) { return true; }

	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);

	// Negative control 1: neither slot resolves a source -- the disclosure must be empty.
	TestTrue(TEXT("T-06: disclosure is empty when neither slot resolves a source"),
		Inspector->GetPendingCorrespondenceDisclosure().IsEmpty());

	// Negative control 2: only the primary slot resolves -- the second slot's EKind::None
	// must still suppress the disclosure entirely (a HEAVY pass with only one side selected
	// cannot report a cost, so it must not).
	Inspector->SetBankForTest(Primary);
	TestTrue(TEXT("T-06: disclosure is empty when only the primary slot resolves a source"),
		Inspector->GetPendingCorrespondenceDisclosure().IsEmpty());

	// Positive control: both slots resolved, and ComputeCorrespondence() has NEVER been
	// called on this Inspector -- this is the exact pre-run moment the CHANGELOG's "before it
	// runs" claim describes, asserted strictly before any compute call exists in this test.
	// Oracle: the tutorial Primary bank is 22 live rows, Secondary is 6 live rows
	// (TutorialBankGeometry.csv -- the same oracle the sibling T-11 cell above derives from),
	// and GetPendingCorrespondenceDisclosure()'s own format string
	// ("HEAVY pass -- cost scales with both banks' sizes (%d live x %d live)") is reproduced
	// here as a full-string oracle rather than a substring check, so the cell fails if either
	// count drifts, not merely if the HEAVY token disappears.
	Inspector->SetSecondBankForTest(Secondary);
	const FString Disclosure = Inspector->GetPendingCorrespondenceDisclosure();
	TestEqual(TEXT("T-06/T-815: pre-run disclosure names the HEAVY pass and both banks' live "
		"counts, read strictly before any ComputeCorrespondence() call"),
		Disclosure,
		FString(TEXT("HEAVY pass -- cost scales with both banks' sizes (22 live x 6 live)")));

	return true;
}

// ===========================================================================
// T-11's uncalled public accessor (a code review found "T-11/C1's UI disclosure string is
// untested despite an existing public seam"). GetArchivePeekGeometry()/GetSecondArchivePeekGeometry()
// are already real, already public, and called by zero tests -- the cheapest fix in the whole
// audit, an assertion away rather than a seam away. Oracle: the tutorial Primary bank is 22
// rows x 8 dims, Cosine, Float32, 2 channels (the sidecar's own header rule,
// TutorialBankGeometry.csv); Secondary is 6 rows x 8 dims, same shape.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSArchivePeekGeometryDisclosureTest,
	"SuperFAISS.D.ArchivePeekGeometryDisclosure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSArchivePeekGeometryDisclosureTest::RunTest(const FString& Parameters)
{
	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);

	TArray<uint8> Bytes;
	if (!BakeAsArchiveBytes(*this, TEXT("Primary"), {}, Bytes)) { return true; }
	TestTrue(TEXT("(setup) primary archive opens"),
		Inspector->OpenScratchArchiveFromBytes(Bytes, TEXT("primary.bin")));

	const FString Geometry = Inspector->GetArchivePeekGeometry();
	TestTrue(TEXT("T-11: peeked geometry reports the archive's real row/dim count (22 x 8)"),
		Geometry.Contains(TEXT("22 x 8")));
	TestTrue(TEXT("T-11: peeked geometry names the metric (Cosine)"), Geometry.Contains(TEXT("Cosine")));
	TestTrue(TEXT("T-11: peeked geometry names the quantization (Float32)"),
		Geometry.Contains(TEXT("Float32")));
	TestTrue(TEXT("T-11: peeked geometry names the channel count (2 channel(s))"),
		Geometry.Contains(TEXT("2 channel")));
	TestFalse(TEXT("T-11: a clean archive with no trailing bytes discloses no trailing-data note"),
		Geometry.Contains(TEXT("trailing")));

	// Trailing-data disclosure: append bytes past the archive's own declared length and
	// re-open. PeekScratchArchiveGuardVitalityTest already proves archiveBytes strictly
	// locates the trailer boundary at the CORE boundary; this proves the WIDGET actually
	// surfaces that fact in the user-visible geometry line (T-11's own UI half, C1's own
	// claim: "the geometry line, including the trailing-data disclosure... shown before the
	// load commits").
	TArray<uint8> WithTrailer = Bytes;
	WithTrailer.Append({0xDE, 0xAD, 0xBE, 0xEF});
	TestTrue(TEXT("(setup) archive with trailing bytes still opens"),
		Inspector->OpenScratchArchiveFromBytes(WithTrailer, TEXT("primary-trailer.bin")));
	const FString TrailerGeometry = Inspector->GetArchivePeekGeometry();
	TestTrue(TEXT("T-11: trailing-data disclosure appears in the geometry line"),
		TrailerGeometry.Contains(TEXT("trailing")));

	// The second slot's own copy of the same public seam, independently.
	TArray<uint8> SecondaryBytes;
	if (!BakeAsArchiveBytes(*this, TEXT("Secondary"), {}, SecondaryBytes)) { return true; }
	TestTrue(TEXT("(setup) secondary archive opens on the second slot"),
		Inspector->OpenSecondScratchArchiveFromBytes(SecondaryBytes, TEXT("secondary.bin")));
	const FString SecondGeometry = Inspector->GetSecondArchivePeekGeometry();
	TestTrue(TEXT("T-11: second slot's peeked geometry reports its own real row/dim count (6 x 8)"),
		SecondGeometry.Contains(TEXT("6 x 8")));

	return true;
}

// ===========================================================================
// C2's Novelty-on-tombstoned-archive composition cell (a coverage audit found this "proven for
// channel-scope, not for Novelty"). Oracle: Primary rows 13/14 carry the SAME tag (3,3) by
// deliberate fixture design (ISO-B / ISO-B-dup) and share it with NO other Primary row (the
// existing oracle test file's own derivation, SuperFAISSTutorialBankOracleTests.cpp) -- row
// 13's identity-limb Duplicate verdict therefore depends ENTIRELY on row 14 being live. This
// is the "provably distinct" discipline the T-03 label test and the T-04 pruned-parity test
// both already use, applied to Novelty: tombstone the ONE row that makes the difference and
// assert the verdict flips, not merely that the call still succeeds.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSTutorialArchiveNoveltyTombstoneCompositionTest,
	"SuperFAISS.D.TutorialArchiveNoveltyTombstoneComposition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSTutorialArchiveNoveltyTombstoneCompositionTest::RunTest(const FString& Parameters)
{
	// Untombstoned archive: row 13's only exact duplicate (tag (3,3)) is row 14, live -> Duplicate.
	{
		TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
		if (!OpenTutorialArchive(*this, Inspector.Get(), TEXT("Primary"), {}, /*bSecondSlot*/ false))
		{
			return true;
		}
		Inspector->ProbeNovelty(TEXT("#13"));
		TestTrue(TEXT("(setup) untombstoned probe #13 valid"), Inspector->GetNoveltyResult().bValid);
		TestTrue(TEXT("C2 composition oracle: row 13's only duplicate (row 14) is live -> Duplicate"),
			Inspector->GetNoveltyResult().Verdict == ESuperFAISSNoveltyVerdict::Duplicate);
	}

	// Tombstoned archive: row 14 removed -> row 13 has no live duplicate anywhere in the bank
	// -> Novelty must NOT classify Duplicate. A tombstone-BLIND identity limb (one that reads
	// the archive's raw bytes without honoring the tombstone) would still see row 14's bytes
	// and wrongly report Duplicate -- this is exactly the class of bug the composition cell
	// exists to catch, proven for channel-scope already (T-04's pruned-parity test) but never
	// for Novelty until this cell.
	{
		TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
		if (!OpenTutorialArchive(*this, Inspector.Get(), TEXT("Primary"), {14}, /*bSecondSlot*/ false))
		{
			return true;
		}
		Inspector->ProbeNovelty(TEXT("#13"));
		TestTrue(TEXT("(setup) tombstoned probe #13 valid"), Inspector->GetNoveltyResult().bValid);
		TestFalse(TEXT("C2 composition (tombstone-respecting): row 13's only duplicate (row 14) is "
			"tombstoned -> Novelty must NOT classify Duplicate"),
			Inspector->GetNoveltyResult().Verdict == ESuperFAISSNoveltyVerdict::Duplicate);
	}

	return true;
}

// ===========================================================================
// The second slot's untested archive legs (a coverage audit found "every oracle-gated archive test
// opens the archive on the primary slot only... the second slot's archive-sourced Novelty,
// channel-scope, and correspondence-leg cells are covered only at the reachability level").
// Novelty has no second-slot entry point at all (ProbeNovelty always reads
// GetPrimarySource() -- confirmed at source in SSuperFAISSBankInspector.cpp; there is no
// second-bank probe capability to test), so the two second-slot cells this round closes are
// channel-scope and the correspondence leg, both of which DO have a second-slot code path.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSTutorialSecondSlotArchiveChannelScopeParityTest,
	"SuperFAISS.D.TutorialSecondSlotArchiveChannelScopeParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSTutorialSecondSlotArchiveChannelScopeParityTest::RunTest(const FString& Parameters)
{
	using namespace superfaiss;

	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	// A channel-carrying ASSET on the PRIMARY slot first -- the projection-scope combo
	// (ProjectionScopes/SetAnalysisScopeForTest's match set) is populated by whichever source
	// first offers channels (the sibling primary-slot tests' own documented reasoning), and the
	// second slot never touches this state at all
	// (SuperFAISS.D.SecondSlotArchiveOpenChannelStateUnchanged) -- opening the second-slot
	// archive alone, with no asset ever selected, leaves ProjectionScopes empty and
	// SetAnalysisScopeForTest below would silently no-op.
	USuperFAISSVectorBank* AssetBank = BakeAsAsset(*this, TEXT("Primary"));
	if (AssetBank == nullptr) { return true; }
	Inspector->SetBankForTest(AssetBank);
	if (!OpenTutorialArchive(*this, Inspector.Get(), TEXT("Primary"), {}, /*bSecondSlot*/ true))
	{
		return true;
	}
	Inspector->SetAnalysisScopeForTest(TEXT("chanA"));
	const FSuperFAISSInspectionSource SecondSource = Inspector->GetSecondSource();
	TestEqual(TEXT("(setup) second source resolves to Archive kind after opening"),
		static_cast<int32>(SecondSource.Kind), static_cast<int32>(FSuperFAISSInspectionSource::EKind::Archive));

	TArray<uint8, TAlignedHeapAllocator<16>> Payload;
	TArray<float> Scales;
	BankView View;
	TArray<int32> SourceIndices;
	const bool bBuilt = Inspector->BuildAnalysisSampleForTest(
		SecondSource, SecondSource.GetCount(), Payload, Scales, View, SourceIndices);
	TestTrue(TEXT("second-slot archive channel-scoped sample build succeeds"), bBuilt);
	if (!bBuilt) { return true; }

	alignas(16) float QueryBuf[4] = {1.0f, 0.0f, 0.0f, 0.0f}; // the exact chanA=0 unit direction
	Workspace Ws;
	Hit Hits[32];
	int32_t HitCount = 0;
	QueryParams Params;
	Params.k = FMath::Min(32, View.count);
	const Status QueryStatus = Query(View, QueryBuf, Params, Ws, Hits, &HitCount);
	TestEqual(TEXT("query over the second-slot channel-scoped view succeeds"),
		static_cast<int>(QueryStatus), static_cast<int>(Status::Ok));

	// Same golden set the primary-slot sibling test (TutorialArchiveChannelScopeParityTest)
	// derives: every Primary row with ChanADir==0 -- {0,1,2,3,11,15,16,20} -- identical
	// geometry, now read through the SECOND slot's own source-resolution path instead of the
	// primary's.
	TSet<int32> ExpectedChanA0 = {0, 1, 2, 3, 11, 15, 16, 20};
	TSet<int32> ActualOnes;
	for (int32 i = 0; i < HitCount; ++i)
	{
		if (!SourceIndices.IsValidIndex(Hits[i].index)) { continue; }
		if (FMath::Abs(Hits[i].score - 1.0f) < 1e-5f)
		{
			ActualOnes.Add(SourceIndices[Hits[i].index]);
		}
	}
	TArray<int32> ActualSorted = ActualOnes.Array();
	TArray<int32> ExpectedSorted = ExpectedChanA0.Array();
	ActualSorted.Sort();
	ExpectedSorted.Sort();
	TestEqual(TEXT("second-slot archive: rows scoring 1.0 match the ChanADir=0 golden set "
		"(dim 4/8 parity, second-slot leg -- untested by every prior oracle-gated cell, "
		"which opened the archive on the primary slot only)"),
		ActualSorted, ExpectedSorted);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSTutorialSecondSlotArchiveCorrespondenceParityTest,
	"SuperFAISS.D.TutorialSecondSlotArchiveCorrespondenceParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSTutorialSecondSlotArchiveCorrespondenceParityTest::RunTest(const FString& Parameters)
{
	FSettingsGuard Guard;
	Guard.Settings->MatchK = 2; // Secondary has only 6 rows; MatchK must not exceed live count
	Guard.Settings->SampleLimit = 64;
	// A confirmation review found (F-4) that this oracle's Matched verdict must not depend on
	// whatever CslsMarginThreshold a developer's local EditorPerProjectUserSettings.ini
	// currently holds -- set explicitly from the compiled literal (never read
	// Guard.OrigThreshold, which is ini-affected) so the cell classifies the same way on
	// every machine.
	Guard.Settings->CslsMarginThreshold = USuperFAISSInspectorSettings::kDefaultCslsMarginThreshold;

	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	USuperFAISSVectorBank* AssetPrimary = BakeAsAsset(*this, TEXT("Primary"));
	if (AssetPrimary == nullptr) { return true; }
	Inspector->SetBankForTest(AssetPrimary);
	Inspector->SetAnalysisScopeForTest(TEXT("chanA"));

	// Secondary, opened as an ARCHIVE on the SECOND slot -- the sibling primary-slot test
	// (TutorialArchiveCorrespondenceParityTest) puts the archive on the primary/A side; this
	// cell is the untested B/second-slot leg the audit names.
	if (!OpenTutorialArchive(*this, Inspector.Get(), TEXT("Secondary"), {}, /*bSecondSlot*/ true))
	{
		return true;
	}
	const FSuperFAISSInspectionSource SecondSource = Inspector->GetSecondSource();
	TestEqual(TEXT("(setup) second source resolves to Archive kind"),
		static_cast<int32>(SecondSource.Kind), static_cast<int32>(FSuperFAISSInspectionSource::EKind::Archive));

	Inspector->ComputeCorrespondence();
	const FSuperFAISSMatchPairResult* Pair = Inspector->GetMatchPairResults().FindByPredicate(
		[](const FSuperFAISSMatchPairResult& P) { return P.SourceIndexA == 4; });
	TestTrue(TEXT("(setup) row 4 has a match-pair result"), Pair != nullptr);
	if (Pair == nullptr) { return true; }
	// Same row4<->Secondary2 oracle the primary-slot sibling test derives (chanA-scoped,
	// MatchK=2 -- see that test's own header comment for the full hand derivation): margin
	// 0.5, Matched at the shipped default threshold.
	TestEqual(TEXT("second-slot leg oracle: row4<->Secondary2 (untested composition per the "
		"audit: every oracle-gated archive test opened the archive on the primary slot only)"),
		Pair->SourceIndexB, 2);
	TestTrue(TEXT("second-slot leg oracle: row4<->Secondary2 classifies Matched"),
		Pair->State == ESuperFAISSMatchState::Matched);
	TestTrue(TEXT("second-slot leg oracle: margin close to the hand-derived 0.5"),
		FMath::Abs(Pair->CslsMargin - 0.5f) < 1e-4f);

	return true;
}

// ===========================================================================
// T-10's tooltip half (plan sec4, SF34-007: "complete tooltip + doc URL"). The plan never
// states which tooltip or what "complete" means beyond that; this cell anchors to the one
// place the code review already specifies precisely (finding 3's own fix language, for THIS
// exact property): "the same clause on the CslsMarginThreshold property tooltip so it is
// present at the point of use" -- the clause being finding 3's own subject, that the
// calibration ran at a smaller MatchK (2) than the shipped production default (10). The
// doc-URL half of T-10 has no existing surface anywhere in the widget (zero LaunchURL calls,
// zero doc-path strings, confirmed by the audit's own grep) -- routed separately, not
// invented here (see the round-2 test-design record section 2).
//
// UPROPERTY comments are captured as the "ToolTip" metadata by UnrealHeaderTool automatically
// (standard engine reflection behavior, present in every editor build this automation test
// runs in) -- no production code change is needed to make this property's CURRENT comment
// readable via FProperty::GetToolTipText(); only its CONTENT is what this cell finds bare.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSCslsMarginThresholdTooltipCompletenessTest,
	"SuperFAISS.D.CslsMarginThresholdTooltipCompleteness",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSCslsMarginThresholdTooltipCompletenessTest::RunTest(const FString& Parameters)
{
	const FProperty* ThresholdProp = FindFProperty<FProperty>(
		USuperFAISSInspectorSettings::StaticClass(),
		GET_MEMBER_NAME_CHECKED(USuperFAISSInspectorSettings, CslsMarginThreshold));
	TestNotNull(TEXT("(setup) CslsMarginThreshold property reflects"), ThresholdProp);
	if (ThresholdProp == nullptr) { return true; }

	const FString Tooltip = ThresholdProp->GetToolTipText().ToString();
	TestTrue(TEXT("T-10: the CslsMarginThreshold tooltip discloses the calibration's own MatchK "
		"cell (the code review's finding 3: the margin distribution is a function of MatchK, "
		"calibrated at MatchK=2 while the shipped production default is MatchK=10 -- the "
		"current comment names the tutorial population and the interval but never the word "
		"\"MatchK\")"),
		Tooltip.Contains(TEXT("MatchK")));

	return true;
}

// ===========================================================================
// T-07 (SF34-007), plan sec10.1 / round-2 test-design sec2: the archive-metadata seam
// GetSourceHeaderLineForTest() (SSuperFAISSBankInspector.h:483) is now built; the review's
// N-3 finding notes it has no caller. Oracle: a tutorial Primary archive with row 15
// tombstoned -- 22 published, live 21 (SuperFAISSTutorialBankOracleTests.cpp's own
// derivation: tombstoning row 15 drops live count to 21) -- Cosine/Float32/2-channel per
// the fixture's own InitWithChannels call. Every one of SourceHeaderLine()'s six
// interpolated fields is pinned as a single full-string oracle, not a substring check, so
// the cell fails on a regression to any one field, not merely on the string going empty.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSTutorialArchiveSourceHeaderLineTest,
	"SuperFAISS.D.TutorialArchiveSourceHeaderLine",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSTutorialArchiveSourceHeaderLineTest::RunTest(const FString& Parameters)
{
	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	if (!OpenTutorialArchive(*this, Inspector.Get(), TEXT("Primary"), {15}, /*bSecondSlot*/ false))
	{
		return true;
	}

	const FString HeaderLine = Inspector->GetSourceHeaderLineForTest();
	TestEqual(TEXT("T-07: the rendered archive metadata line names the display name, published "
		"count, dims, live count (22 published, 21 live after tombstoning row 15), metric, "
		"quantization, and channel count -- exactly SourceHeaderLine()'s own format string, "
		"every field pinned"),
		HeaderLine,
		FString(TEXT("Primary-archive: 22 x 8 (live 21), metric Cosine, quant Float32, 2 channel(s)")));

	return true;
}

// ===========================================================================
// T-10's doc-URL half (plan sec10.2): CorrespondenceDocumentationUrl is built but the
// review's N-3 finding notes it has no caller. Round-2 closeout (final review S-4/M-1/M-2,
// 2026-07-25) rewrites and extends this cell: assertions (a)/(b) below are unchanged from
// N-3's authoring; (c) and (d) are new.
//   (a) the constant's exact string value, matching plan sec10.2's specified URL character
//       for character -- a copy-paste guard, catches an accidental edit to the constant, but
//       cannot catch the heading moving out from under it, because the heading is not an
//       input to this assertion;
//   (b) the claim the constant's own surrounding prose makes -- that the linked README
//       section documents CslsMarginThreshold's calibration -- checked IN-PROCESS by reading
//       the plugin's own README.md off disk and confirming the "Bank Inspector" section
//       actually contains the calibration content (the "0.375" value and the word
//       "calibrat[ed/ion]"). This is the exact defect N-1 found (three surfaces promised
//       content the section did not carry) turned into a standing regression guard for the
//       PROSE. It is a prefix match on the heading text and, like (a), is not an input to the
//       heading itself -- it does not guard the anchor;
//   (c) S-4 (final review, 2026-07-25): GitHub's heading-to-slug rule, computed in-process
//       against the README's ACTUAL heading text and compared to the constant's own fragment
//       -- the failure (a) and (b) cannot catch. Concretely: renaming the heading to
//       "## Bank Inspector -- structure and correspondence" changes GitHub's real anchor to
//       "bank-inspector--structure-and-correspondence"; the constant's fragment does not
//       follow; GitHub renders an unmatched fragment at the top of the page rather than
//       erroring; (a), (b), and every other of the suite's 126 cells stay green while the
//       shipped hyperlink silently drops the reader at the top of a 500-line README instead
//       of at the calibration prose its tooltip promises. (c) is the guard for exactly that;
//   (d) M-2 (final review, 2026-07-25): the hyperlink WIDGET is reachable in the panel's
//       built layout, not merely wired in source -- plan sec10.2's reachability assertion,
//       never authored before this round.
// Not tested: an actual browser navigation via FPlatformProcess::LaunchURL (unreachable from
// an automation test by construction -- it launches a real OS-level browser process) and a
// live GitHub fetch confirming the rendered page actually scrolls to the anchor (a network
// call, not a unit test) -- (c) proves the anchor is correctly DERIVED, not that GitHub's
// live renderer accepts it.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSCorrespondenceDocumentationUrlTest,
	"SuperFAISS.D.CorrespondenceDocumentationUrl",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSCorrespondenceDocumentationUrlTest::RunTest(const FString& Parameters)
{
	// (a) the constant, character for character.
	TestEqual(TEXT("T-10: CorrespondenceDocumentationUrl matches plan sec10.2's specified URL"),
		FString(SSuperFAISSBankInspector::CorrespondenceDocumentationUrl),
		FString(TEXT("https://github.com/dansupergameprogrammer/superfaiss-unreal/blob/main/"
			"SuperFAISSUnreal/README.md#bank-inspector--structure-novelty-and-correspondence-v32")));

	// (b) the surrounding claim, checked against the actual document: read this plugin's own
	// README.md (the corresponding file in this checkout's own repository -- the URL targets
	// the same-named file one sync step away, in the published repository; C-1's finding in
	// the 3.3.1 final review is the standing record that the two are not guaranteed to agree)
	// and confirm the "Bank Inspector" section it links to genuinely carries the calibration
	// content the tooltip (SSuperFAISSBankInspector.cpp), the changelog, and the header
	// comment all promise.
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SuperFAISSUnreal"));
	TestTrue(TEXT("(setup) SuperFAISSUnreal plugin resolves"), Plugin.IsValid());
	if (!Plugin.IsValid()) { return true; }

	const FString ReadmePath = FPaths::Combine(Plugin->GetBaseDir(), TEXT("README.md"));
	FString Readme;
	TestTrue(TEXT("(setup) README.md loads from the plugin's own base directory"),
		FFileHelper::LoadFileToString(Readme, *ReadmePath));
	if (Readme.IsEmpty()) { return true; }

	const int32 HeadingStart = Readme.Find(TEXT("## Bank Inspector"));
	TestTrue(TEXT("(setup) the 'Bank Inspector' section heading exists in README.md"), HeadingStart != INDEX_NONE);
	if (HeadingStart == INDEX_NONE) { return true; }

	int32 NextHeading = Readme.Find(TEXT("\n## "), ESearchCase::CaseSensitive, ESearchDir::FromStart,
		HeadingStart + 3);
	if (NextHeading == INDEX_NONE) { NextHeading = Readme.Len(); }
	const FString Section = Readme.Mid(HeadingStart, NextHeading - HeadingStart);

	TestTrue(TEXT("N-1 regression guard: the linked README section states the calibrated "
		"threshold's value (0.375) -- the exact defect N-1 found was this being absent"),
		Section.Contains(TEXT("0.375")));
	TestTrue(TEXT("N-1 regression guard: the linked README section names the calibration by "
		"word, not merely by number"),
		Section.Contains(TEXT("calibrat")));

	// (c) S-4: GitHub's heading-to-slug rule, applied in-process to the README's ACTUAL
	// heading text (not the "## Bank Inspector" prefix (b) matched on), compared against the
	// URL constant's fragment. GitHub's rule is mechanical and reproduced here without any
	// network call: lowercase the text; keep only characters in [a-z0-9 -], dropping every
	// other character (this is the step that removes the em dash, the commas, the parentheses,
	// and the period in the current heading); then replace each remaining space with a
	// hyphen. Repeated hyphens are NOT collapsed and the result is NOT trimmed -- the shipped
	// fragment's own double hyphen after "inspector" (where the heading's em dash sat between
	// two spaces) is the standing proof the real rule does neither; collapsing or trimming
	// here would make this assertion pass against a slug GitHub itself would not generate.
	const int32 HeadingLineEnd = Readme.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart,
		HeadingStart);
	const FString HeadingLine = (HeadingLineEnd == INDEX_NONE)
		? Readme.Mid(HeadingStart)
		: Readme.Mid(HeadingStart, HeadingLineEnd - HeadingStart);
	const FString HeadingText = HeadingLine.RightChop(3); // strip the leading "## "

	FString ComputedSlug;
	ComputedSlug.Reserve(HeadingText.Len());
	for (const TCHAR Ch : HeadingText.ToLower())
	{
		if ((Ch >= TEXT('a') && Ch <= TEXT('z')) || (Ch >= TEXT('0') && Ch <= TEXT('9')) ||
			Ch == TEXT(' ') || Ch == TEXT('-'))
		{
			ComputedSlug.AppendChar(Ch);
		}
	}
	ComputedSlug = ComputedSlug.Replace(TEXT(" "), TEXT("-"));

	const FString Url(SSuperFAISSBankInspector::CorrespondenceDocumentationUrl);
	const int32 FragmentIndex = Url.Find(TEXT("#"));
	TestTrue(TEXT("(setup) CorrespondenceDocumentationUrl contains a '#' fragment"), FragmentIndex != INDEX_NONE);
	if (FragmentIndex == INDEX_NONE) { return true; }
	const FString UrlFragment = Url.Mid(FragmentIndex + 1);

	TestEqual(FString::Printf(TEXT("S-4: GitHub's heading-to-slug rule applied to README.md's actual "
		"heading text ('%s') computes '%s'; this must equal CorrespondenceDocumentationUrl's "
		"fragment ('%s') -- if the heading changes, the URL's fragment must change with it in "
		"the same commit, or the link silently stops resolving to the calibration prose"),
		*HeadingText, *ComputedSlug, *UrlFragment),
		ComputedSlug, UrlFragment);

	// (d) M-2 / plan sec10.2: the hyperlink WIDGET is reachable in the panel's built layout,
	// not merely wired in source. T-11's reachability cell (above) proves reachability for a
	// data ACCESSOR by calling the real public seam and asserting on its content; no
	// equivalent public accessor exists for "is the hyperlink in the built layout" (there is
	// no test-support seam for it), so the same idiom applied to a WIDGET is a Slate-tree walk
	// from the constructed panel down to its children, checking each node's real Slate type
	// via the engine's own SWidget::GetTypeAsString()/GetChildren() -- the real path, not a
	// re-derivation of the answer.
	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	bool bFoundHyperlink = false;
	TFunction<void(TSharedRef<SWidget>)> Walk = [&bFoundHyperlink, &Walk](TSharedRef<SWidget> Widget)
	{
		if (bFoundHyperlink) { return; }
		if (Widget->GetTypeAsString() == TEXT("SHyperlink")) { bFoundHyperlink = true; return; }
		FChildren* Children = Widget->GetChildren();
		for (int32 i = 0; i < Children->Num() && !bFoundHyperlink; ++i)
		{
			Walk(Children->GetChildAt(i));
		}
	};
	Walk(Inspector);
	TestTrue(TEXT("M-2/plan sec10.2: the Documentation hyperlink widget is reachable in the "
		"panel's built layout, not merely wired in source"), bFoundHyperlink);

	return true;
}

// ===========================================================================
// Item 7 / the code review's finding 4, plan sec10.3: kDefaultCslsMarginThreshold is built
// (SuperFAISSInspectorSettings.h) but the review's N-3 finding notes no test reads it --
// the header comment claiming otherwise is itself stale as of this cell landing. Two
// assertions, exactly as the plan and the review's own fix text specify: (a) the literal
// lies in the tutorial population's calibration interval (0.25, 0.5]; (b) explicitly
// setting CslsMarginThreshold FROM THE LITERAL (never from Guard.OrigThreshold, the
// ini-affected CDO field) reproduces the two-cluster split
// (TutorialCorrespondenceCalibrationTest's own row11/row13 oracle pairs, MatchK=2). Both
// fail on a header regression regardless of local EditorPerProjectUserSettings.ini state,
// which is what "independent of ini state" requires.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSCslsMarginThresholdLiteralPinTest,
	"SuperFAISS.D.CslsMarginThresholdLiteralPin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSCslsMarginThresholdLiteralPinTest::RunTest(const FString& Parameters)
{
	// (a) the compiled literal lies in the tutorial population's own calibration interval.
	TestTrue(TEXT("The code review's finding 4 (a): kDefaultCslsMarginThreshold lies in (0.25, 0.5], the "
		"tutorial-bank correspondence population's calibration interval"),
		USuperFAISSInspectorSettings::kDefaultCslsMarginThreshold > 0.25f &&
		USuperFAISSInspectorSettings::kDefaultCslsMarginThreshold <= 0.5f);

	// (b) set FROM THE LITERAL directly -- never through NewObject<>() or the CDO, both of
	// which are ini-affected -- and confirm it reproduces the two-cluster split.
	FSettingsGuard Guard;
	Guard.Settings->MatchK = 2;
	Guard.Settings->SampleLimit = 64;
	Guard.Settings->CslsMarginThreshold = USuperFAISSInspectorSettings::kDefaultCslsMarginThreshold;

	USuperFAISSVectorBank* Primary = BakeAsAsset(*this, TEXT("Primary"));
	USuperFAISSVectorBank* Secondary = BakeAsAsset(*this, TEXT("Secondary"));
	if (Primary == nullptr || Secondary == nullptr) { return true; }

	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	Inspector->SetBankForTest(Primary);
	Inspector->SetSecondBankForTest(Secondary);
	Inspector->ComputeCorrespondence();

	const FSuperFAISSMatchPairResult* Pair11 = Inspector->GetMatchPairResults().FindByPredicate(
		[](const FSuperFAISSMatchPairResult& P) { return P.SourceIndexA == 11; });
	const FSuperFAISSMatchPairResult* Pair13 = Inspector->GetMatchPairResults().FindByPredicate(
		[](const FSuperFAISSMatchPairResult& P) { return P.SourceIndexA == 13; });
	TestTrue(TEXT("(setup) row 11 and row 13 both have match-pair results"),
		Pair11 != nullptr && Pair13 != nullptr);
	if (Pair11 == nullptr || Pair13 == nullptr) { return true; }

	TestTrue(TEXT("The code review's finding 4 (b): at the LITERAL default, the clean singleton pair "
		"(row11<->Sec1, margin ~0.5) classifies Matched"),
		Pair11->State == ESuperFAISSMatchState::Matched);
	TestTrue(TEXT("The code review's finding 4 (b): at the LITERAL default, the near-duplicate pair "
		"(row13<->Sec3, margin ~0.25) classifies Ambiguous -- the distinction the margin "
		"exists to draw, reproduced from the compiled constant independent of any local "
		"ini override"),
		Pair13->State == ESuperFAISSMatchState::Ambiguous);

	return true;
}

// ===========================================================================
// SuperFAISS.D.ReadmeTestCountAssertion (round-2 closeout item 1, final review S-1/S-2's
// structural fix): the README's own test-count claim, checked by something that fails rather
// than maintained by hand. The same hand-written pair of numbers drifted twice in one day --
// f639bc1e4d corrected 113->119 / 117->123; 92ea7569b0 added three cells and left both numbers
// stale three commits later, inside the same reviewed range. Both counts below are taken at
// runtime, never by construction (StandardsDocument.md sec5.4): the registered-test count
// from FAutomationTestFramework::Get().GetValidTestNames() -- the automation framework's own
// registry, process-global -- and the claimed counts parsed out of the README's own two
// sentences with FRegexPattern/FRegexMatcher.
//
// Both of the README's numbers are asserted, not merely the plugin-only half: this editor
// module's process is WizardGame.uproject, whose .uplugin/.uproject enables
// SuperFAISSUnrealMCP unconditionally (confirmed by reading WizardGame.uproject's own Plugins
// list), and the automation registry is process-global, so GetValidTestNames() already
// returns the MCP module's SuperFAISS.M.* tests in this same run without this module
// depending on the MCP module -- the build-verification record for this closeout observed
// exactly that composition (122 plugin + 4 MCP = 126). A cell that asserted only the
// plugin-only count would be the exact half-claim failure mode named for this item: it does
// not reach D:\SuperFAISSUnreal\README.md (final review S-2's file, a different repository
// this in-tree automation run cannot see) -- that number stays a hand-maintained fact
// re-derived at the point the release process already runs this suite against the synced
// tree, the same execution C-1's remedy performs.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSReadmeTestCountAssertionTest,
	"SuperFAISS.D.ReadmeTestCountAssertion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSReadmeTestCountAssertionTest::RunTest(const FString& Parameters)
{
	// The registry count: every currently-registered SuperFAISS.* automation test, read from
	// the framework itself at runtime -- never by construction. Two engine behaviors this cell
	// must account for, both confirmed by reading Engine/Source/Runtime/Core (AutomationTest.h/
	// .cpp) and both confirmed by execution below (the cell's first-authored version, without
	// either fix, read a registry of zero SuperFAISS.* tests and failed for that reason instead
	// of the one it exists to catch):
	//
	// (1) FAutomationTestInfo::GetTestName() is NOT the dotted display path ("SuperFAISS.D...")
	//     -- it is FAutomationTestBase's internal TestName, which IMPLEMENT_SIMPLE_AUTOMATION_
	//     TEST's own instance declaration constructs from the C++ CLASS name
	//     (`TClass TClass##AutomationTestInstance(TEXT(#TClass))`, e.g.
	//     "FSuperFAISSReadmeTestCountAssertionTest"), not from the macro's PrettyName argument.
	//     The dotted path used everywhere else in this suite -- and the one the README's own
	//     claim is stated in -- is FAutomationTestInfo::GetFullTestPath().
	// (2) GetValidTestNames() filters candidates against the framework's own "requested test
	//     filter", which the framework constructs defaulted to SmokeFilter and which is
	//     otherwise only ever set transiently (the Automation UI's filter checkboxes;
	//     RunSmokeTests's own TGuardValue, which restores its prior value on return) -- neither
	//     active during a single headless "-ExecCmds=Automation RunTests ..." pass. Every
	//     SuperFAISS.* test in this suite is declared EditorContext|ProductFilter, which shares
	//     no bit with SmokeFilter, so left at the default this enumeration would silently
	//     return zero of this suite's own tests. Broadened to every filter category for the
	//     duration of this read only, then restored to the constructed default -- there is no
	//     public getter to save and restore whatever value was live before this call, but no
	//     other call site in this codebase persists a value other than that default.
	FAutomationTestFramework& Framework = FAutomationTestFramework::Get();
	Framework.SetRequestedTestFilter(EAutomationTestFlags_FilterMask);
	TArray<FAutomationTestInfo> TestInfo;
	Framework.GetValidTestNames(TestInfo);
	Framework.SetRequestedTestFilter(EAutomationTestFlags::SmokeFilter);

	int32 TotalCount = 0;
	int32 McpCount = 0;
	for (const FAutomationTestInfo& Info : TestInfo)
	{
		if (Info.GetFullTestPath().StartsWith(TEXT("SuperFAISS.")))
		{
			++TotalCount;
			if (Info.GetFullTestPath().StartsWith(TEXT("SuperFAISS.M")))
			{
				++McpCount;
			}
		}
	}
	const int32 PluginCount = TotalCount - McpCount;

	// The claimed count: parsed out of the plugin's own README.md, the same file and load
	// idiom FSuperFAISSCorrespondenceDocumentationUrlTest already uses above.
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SuperFAISSUnreal"));
	TestTrue(TEXT("(setup) SuperFAISSUnreal plugin resolves"), Plugin.IsValid());
	if (!Plugin.IsValid()) { return true; }

	const FString ReadmePath = FPaths::Combine(Plugin->GetBaseDir(), TEXT("README.md"));
	FString Readme;
	TestTrue(TEXT("(setup) README.md loads from the plugin's own base directory"),
		FFileHelper::LoadFileToString(Readme, *ReadmePath));
	if (Readme.IsEmpty()) { return true; }

	const FRegexPattern PluginCountPattern(TEXT("(\\d+) in this plugin"));
	FRegexMatcher PluginCountMatcher(PluginCountPattern, Readme);
	const bool bPluginCountFound = PluginCountMatcher.FindNext();
	TestTrue(TEXT("README.md states the plugin-only test count in its documented sentence "
		"('N in this plugin') -- a missing match means the sentence was reworded, which is "
		"itself a finding, not a silent skip"), bPluginCountFound);
	if (!bPluginCountFound) { return true; }
	const int32 ClaimedPluginCount = FCString::Atoi(*PluginCountMatcher.GetCaptureGroup(1));

	const FRegexPattern McpCountPattern(TEXT("(\\d+) with the MCP plugin enabled"));
	FRegexMatcher McpCountMatcher(McpCountPattern, Readme);
	const bool bMcpCountFound = McpCountMatcher.FindNext();
	TestTrue(TEXT("README.md states the MCP-inclusive test count in its documented sentence "
		"('N with the MCP plugin enabled') -- a missing match means the sentence was "
		"reworded, which is itself a finding, not a silent skip"), bMcpCountFound);
	if (!bMcpCountFound) { return true; }
	const int32 ClaimedTotalCount = FCString::Atoi(*McpCountMatcher.GetCaptureGroup(1));

	TestEqual(FString::Printf(TEXT("README.md's plugin-only count ('%d in this plugin') must "
		"equal the number of registered SuperFAISS.* tests outside the MCP module's own "
		"SuperFAISS.M group (%d)"), ClaimedPluginCount, PluginCount),
		ClaimedPluginCount, PluginCount);
	TestEqual(FString::Printf(TEXT("README.md's MCP-inclusive count ('%d with the MCP plugin "
		"enabled') must equal every registered SuperFAISS.* test in this process, MCP module "
		"included (%d)"), ClaimedTotalCount, TotalCount),
		ClaimedTotalCount, TotalCount);

	return true;
}

// ===========================================================================
// Round-2 closeout (2026-07-26): the red suite for the two confirmed defects from an
// independent outside code review fixed in production without their own coverage --
// P1 and P2 in that pass's own build log -- and the settlement of the one question that
// review's own account left open (whether the
// SECOND archive slot omits a stale-state reset by design or by gap).
//
// P2 -- USuperFAISSScratchBank::MeasureChannelFrameBytes's two-layer null/invalid-span guard
// (FByteReader::Read, SuperFAISSScratchBank.cpp:50; the public entry's own explicit check,
// :609). Four boundary legs of the header's documented contract ("0 if... Data/Len describe no
// frame at all"): null Data with a positive Len, valid Data with Len==0, valid Data with a
// negative Len, and valid Data with a Len shorter than the real frame (truncated). Every "valid
// Data" leg points INTO the same live buffer a full frame was saved into, at a shorter/zero/
// negative claimed Len -- never a separately-allocated short copy -- so a guard regression that
// reads past the claimed Len finds REAL frame bytes physically present right after it and
// parses them successfully instead of failing, which is exactly the read-past-the-declared-span
// defect class this cell exists to catch (proven by mutation -- see the round-2 test-design
// record's mutation-evidence section; the null leg was confirmed to crash with BOTH guard
// layers removed together, since either guard alone still catches it -- belt and suspenders --
// and the zero/negative legs were confirmed to return the REAL frame's nonzero length, not 0,
// once BOTH the new guards and the pre-existing Pos+N>Len bounds check were disabled together,
// because Data legitimately points at real, well-formed frame bytes beyond the claimed Len).
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSMeasureChannelFrameBytesInvalidSpanTest,
	"SuperFAISS.D.MeasureChannelFrameBytesInvalidSpan",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSMeasureChannelFrameBytesInvalidSpanTest::RunTest(const FString& Parameters)
{
	using namespace superfaiss;

	constexpr int32 Count = 4;
	constexpr int32 Dims = 8;
	const TArray<FName> Names = {TEXT("chanA"), TEXT("chanB")};
	const TArray<int32> Offsets = {0, 4};
	const TArray<int32> Lengths = {4, 4};

	USuperFAISSScratchBank* Scratch = NewObject<USuperFAISSScratchBank>();
	TestTrue(TEXT("(setup) InitWithChannels succeeds"),
		Scratch->InitWithChannels(Count, Dims, ESuperFAISSBankMetric::Cosine,
			ESuperFAISSBankQuantization::Float32, Names, Offsets, Lengths));

	TArray<float> Row;
	Row.SetNumUninitialized(Dims);
	for (int32 R = 0; R < Count; ++R)
	{
		for (int32 D = 0; D < Dims; ++D) { Row[D] = static_cast<float>(R * Dims + D + 1); }
		int32 Index = INDEX_NONE;
		TestTrue(TEXT("(setup) append"), Scratch->Append(Row, Index));
	}

	TArray<uint8> Bytes;
	TestTrue(TEXT("(setup) SaveToBytes"), Scratch->SaveToBytes(Bytes));

	ScratchArchiveInfo Info;
	TestEqual(TEXT("(setup) PeekScratchArchive succeeds"),
		static_cast<int32>(PeekScratchArchive(Bytes.GetData(), Bytes.Num(), &Info)),
		static_cast<int32>(Status::Ok));
	TestEqual(TEXT("(setup) archive declares 2 channels"), Info.channelCount, 2);

	const uint8* FrameStart = Bytes.GetData() + Info.archiveBytes;
	const int64 FrameLen = static_cast<int64>(Bytes.Num()) - Info.archiveBytes;
	TestTrue(TEXT("(setup) the channel-name frame is at least 16 bytes (room for a real "
		"truncation)"), FrameLen >= 16);

	// (setup) positive control: a genuinely well-formed frame, at its own real Len, still
	// measures correctly -- confirms the fixture itself is sound before the four negative legs
	// below are read against the same buffer.
	TestEqual(TEXT("(setup) a well-formed frame at its own Len measures its own real length"),
		USuperFAISSScratchBank::MeasureChannelFrameBytes(FrameStart, FrameLen, 2), FrameLen);

	// Leg 1: Data == nullptr, Len positive. The defect this leg pins: before the P2 fix, Data
	// == nullptr with Len >= N passed the bounds check unchanged and Memcpy read from a null
	// pointer -- this is the crash the outside review found, reachable directly from this
	// public API.
	TestEqual(TEXT("P2 leg 1: null Data with a positive Len returns 0, never a guess"),
		USuperFAISSScratchBank::MeasureChannelFrameBytes(nullptr, FrameLen, 2),
		static_cast<int64>(0));

	// Leg 2: Data valid (points at the real frame), Len == 0. Data has real frame bytes
	// physically present at and after this pointer -- the header's own documented contract
	// requires 0 regardless of what is physically present past the caller's declared Len.
	TestEqual(TEXT("P2 leg 2: valid Data with Len == 0 returns 0, never a guess"),
		USuperFAISSScratchBank::MeasureChannelFrameBytes(FrameStart, 0, 2),
		static_cast<int64>(0));

	// Leg 3: Data valid, Len negative.
	TestEqual(TEXT("P2 leg 3: valid Data with a negative Len returns 0, never a guess"),
		USuperFAISSScratchBank::MeasureChannelFrameBytes(FrameStart, -1, 2),
		static_cast<int64>(0));

	// Leg 4: Data valid, Len positive but shorter than the real frame (truncated). FrameStart
	// still points into the SAME live buffer that carries the real, longer frame right after
	// the claimed Len -- so this leg proves the function honors the CALLER'S declared Len
	// rather than reading whatever is physically present past it.
	const int64 TruncatedLen = FrameLen / 2;
	TestTrue(TEXT("(setup) the truncation leg's Len is shorter than the real frame"),
		TruncatedLen > 0 && TruncatedLen < FrameLen);
	TestEqual(TEXT("P2 leg 4: valid Data with a truncated Len returns 0, never a guess (this leg "
		"already passes today via ReadChannelFrame's own short-read handling -- included so the "
		"cell documents all four boundary legs of the header's contract together)"),
		USuperFAISSScratchBank::MeasureChannelFrameBytes(FrameStart, TruncatedLen, 2),
		static_cast<int64>(0));

	return true;
}

// ===========================================================================
// P1 (round-2, case 1 of the two the finding named): OpenScratchArchiveFromBytes()'s
// conditional projection-scope resync guard (SSuperFAISSBankInspector.cpp, the
// bComboWasEmptyBeforeOpen || bPreviousPrimaryWasArchive condition -- runs when
// ChannelSliderNames.Num() == 0, or when the source that populated it was itself a primary
// archive, T-1100). Case 1: a fresh widget
// where NO source -- asset or archive -- has ever populated the combo. The guard's own
// condition is exactly this state, so the archive's own channels must populate the combo the
// same way OnBankSelected() would for an asset.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSArchiveOpenChannelStateFreshTest,
	"SuperFAISS.D.ArchiveOpenChannelStateFresh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSArchiveOpenChannelStateFreshTest::RunTest(const FString& Parameters)
{
	using namespace superfaiss;
	constexpr int32 Count = 6;
	constexpr int32 Dims = 8;
	const TArray<FName> Names = {TEXT("chanA"), TEXT("chanB")};
	const TArray<int32> Offsets = {0, 4};
	const TArray<int32> Lengths = {4, 4};

	TArray<uint8> Bytes;
	if (!MakeGapClosureArchiveBytes(*this, Count, Dims, Names, Offsets, Lengths, 0xF001ull, Bytes))
	{
		return true;
	}

	// Fresh widget -- no SetBankForTest call at all, no asset ever selected. The guard's own
	// condition (ChannelSliderNames.Num() == 0 && ChannelWeights.Num() == 0) is true from
	// construction.
	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	// Construct() itself calls RefreshBankList()+OnBankSelected() unconditionally
	// (SSuperFAISSBankInspector.cpp:463, :882) -- RefreshBankList() enumerates the REAL asset
	// registry, which may resolve a default bank selection from an unrelated real asset already
	// in the project (e.g. a demo/golden bank used by a sibling test suite) before this test
	// gets a chance to run. SetBankForTest(nullptr) is this suite's own established idiom for a
	// deterministic no-selection state (SuperFAISSInspectorPanelTests.cpp:447,
	// SuperFAISS.D.InspectorNoBankRejection's own setup) -- it clears BankNames/BankAssets and
	// re-runs OnBankSelected() with GetSelectedBank() == nullptr, forcing ChannelSliderNames/
	// ChannelWeights to genuinely empty regardless of whatever Construct() auto-selected. This
	// is the only way to reach the guard's OWN precondition -- "no source has EVER populated
	// the combo" -- deterministically in this harness; a bare SNew() alone is not sufficient,
	// confirmed by execution (this test read a stale, non-"chanA" scope combo left over from
	// Construct()'s own default asset-registry selection before this line was added).
	Inspector->SetBankForTest(nullptr);
	TestTrue(TEXT("archive opens"),
		Inspector->OpenScratchArchiveFromBytes(Bytes, TEXT("fresh-channel-archive.bin")));

	// Proof: the scope combo now knows the archive's own channel name -- SetAnalysisScopeForTest
	// silently no-ops when the name is absent, so a
	// scoped sample build with the RIGHT dims (the channel's own length, 4, not the whole row's,
	// 8) is the only way to observe this without a direct field getter (there is none --
	// ChannelSliderNames/ChannelWeights/ProjectionScopes are private; this is the same idiom
	// SuperFAISS.D.InspectorArchiveChannelScopeSupported already uses to prove the combo's
	// content).
	Inspector->SetAnalysisScopeForTest(TEXT("chanA"));
	TArray<uint8, TAlignedHeapAllocator<16>> Payload;
	TArray<float> Scales;
	BankView View;
	TArray<int32> SourceIndices;
	const bool bBuilt = Inspector->BuildAnalysisSampleForTest(Inspector->GetPrimarySource(),
		Count, Payload, Scales, View, SourceIndices);
	TestTrue(TEXT("P1 fresh-open: the chanA scope resolves against the archive (the combo was "
		"populated from it -- no source had ever populated it before)"), bBuilt);
	if (bBuilt)
	{
		TestEqual(TEXT("P1 fresh-open: the sampled view's dims equal chanA's own length (4), not "
			"the whole row's (8) -- proves the combo holds the archive's real channel, not a "
			"stale or empty scope"), View.dims, 4);
	}

	return true;
}

// ===========================================================================
// P1 (round-2, case 2 of the two the finding named): opening a channel-carrying archive AFTER
// an unrelated asset selection. Two sub-cases, both named in this round's own brief ("an asset
// with DIFFERENT, or ZERO, channels"):
//
// Sub-case A (different channels): the asset's own channel populates the combo
// (ChannelSliderNames.Num() == 1 != 0), so the guard correctly SKIPS the rebuild -- the
// archive's own, different channel name never enters the combo, and the asset's stale name
// stays selectable even though it no longer resolves against the now-primary archive. This is
// the already-pinned asset-driven design (SuperFAISS.D.InspectorArchiveChannelScopeSupported,
// SuperFAISS.D.ClaimsVsCodeCapabilityMatrix), reproduced here with DIFFERENT channel names on
// each side (those two pinned tests use the SAME name on both sides) to prove the guard's own
// condition -- combo non-empty -- not merely a coincidence of matching names.
//
// Sub-case B (zero channels): a zero-channel asset selection leaves ChannelSliderNames and
// ChannelWeights BOTH empty after OnBankSelected() (RepopulateChannelState()'s loop runs zero
// times) -- observationally IDENTICAL, to the guard's own Num()==0 condition, to "no asset ever
// selected". The guard therefore DOES repopulate from the archive in this sub-case, contrary to
// a literal reading of this round's own build log (P1's test-author specification: "must
// leave the scope combo exactly as the asset selection left it", stated without distinguishing the
// different-channel and zero-channel sub-cases). Determined at source and confirmed by
// execution (see the round-2 test-design record) that this sub-case's actual behavior is the
// ARCHIVE's channels populating the combo -- and that this is NOT a defect: a zero-channel
// selection leaves nothing in the combo for a rebuild to disturb, so repopulating from the
// archive's real channels is strictly more useful than leaving the combo at "(whole row)" only.
// This cell pins the CORRECT, executed behavior for sub-case B, not the build log's literal
// wording, per this round's own brief.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSArchiveOpenChannelStateAfterUnrelatedAssetTest,
	"SuperFAISS.D.ArchiveOpenChannelStateAfterUnrelatedAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSArchiveOpenChannelStateAfterUnrelatedAssetTest::RunTest(const FString& Parameters)
{
	using namespace superfaiss;
	constexpr int32 Count = 6;
	constexpr int32 Dims = 8;
	const TArray<FName> ArchiveNames = {TEXT("chanA"), TEXT("chanB")};
	const TArray<int32> ArchiveOffsets = {0, 4};
	const TArray<int32> ArchiveLengths = {4, 4};

	TArray<uint8> ArchiveBytes;
	if (!MakeGapClosureArchiveBytes(*this, Count, Dims, ArchiveNames, ArchiveOffsets,
		ArchiveLengths, 0xF002ull, ArchiveBytes))
	{
		return true;
	}

	// Sub-case A: a DIFFERENT single channel on the asset ("chanX"), never on the archive.
	{
		USuperFAISSVectorBank* DifferentAsset = MakeGapClosureAsset(*this, Count, Dims,
			{TEXT("chanX")}, {0}, {4}, 0xA001ull);
		if (DifferentAsset == nullptr) { return true; }

		TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
		Inspector->SetBankForTest(DifferentAsset); // populates the combo with "chanX" only
		TestTrue(TEXT("archive opens after the different-channel asset"),
			Inspector->OpenScratchArchiveFromBytes(ArchiveBytes, TEXT("unrelated-archive-a.bin")));

		// The archive's OWN channel name never entered the combo -- SetAnalysisScopeForTest
		// silently no-ops, leaving the default "(whole row)" scope, so a sample build against
		// the (now-primary) archive comes back at the WHOLE ROW's dims (8), not chanA's (4).
		Inspector->SetAnalysisScopeForTest(TEXT("chanA"));
		TArray<uint8, TAlignedHeapAllocator<16>> Payload;
		TArray<float> Scales;
		BankView View;
		TArray<int32> SourceIndices;
		const bool bBuilt = Inspector->BuildAnalysisSampleForTest(Inspector->GetPrimarySource(),
			Count, Payload, Scales, View, SourceIndices);
		TestTrue(TEXT("P1 sub-case A: whole-row sample build still succeeds against the archive "
			"(the scope silently stayed at its asset-selection default)"), bBuilt);
		if (bBuilt)
		{
			TestEqual(TEXT("P1 sub-case A: dims equal the WHOLE ROW's length (8) -- chanA was "
				"never added to the combo, proving the guard skipped the rebuild"), View.dims, 8);
		}

		// The asset's OWN stale name is still selectable (the combo was never touched) --
		// applying it against the archive (which has no "chanX" channel) is the documented
		// asset-driven design's own honest consequence: the design note doesn't promise the
		// stale scope remains APPLICABLE, only that the combo's CONTENT is undisturbed.
		Inspector->SetAnalysisScopeForTest(TEXT("chanX"));
		const bool bBuiltStale = Inspector->BuildAnalysisSampleForTest(Inspector->GetPrimarySource(),
			Count, Payload, Scales, View, SourceIndices);
		TestFalse(TEXT("P1 sub-case A: the asset's stale channel name, still selected, does NOT "
			"resolve against the archive's own (different) channel table -- confirms the combo's "
			"content is untouched, literally including a now-inapplicable name, not merely "
			"coincidentally consistent"), bBuiltStale);
	}

	// Sub-case B: a ZERO-channel asset.
	{
		USuperFAISSVectorBank* ZeroChannelAsset = MakeGapClosureAsset(*this, Count, Dims,
			{}, {}, {}, 0xA002ull);
		if (ZeroChannelAsset == nullptr) { return true; }

		TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
		Inspector->SetBankForTest(ZeroChannelAsset); // combo stays at "(whole row)" only
		TestTrue(TEXT("archive opens after the zero-channel asset"),
			Inspector->OpenScratchArchiveFromBytes(ArchiveBytes, TEXT("unrelated-archive-b.bin")));

		// The guard's own condition (ChannelSliderNames.Num() == 0 && ChannelWeights.Num() ==
		// 0) is true after a zero-channel asset selection exactly as it is after no selection at
		// all -- so the archive's channels DO populate the combo here. This is the correct,
		// executed behavior (see the round-2 test-design record's determination); it is NOT the
		// "combo exactly as the asset left it" a literal reading of the build log's own prose
		// would predict, but it is provably harmless: a zero-channel selection left nothing in
		// the combo to preserve.
		Inspector->SetAnalysisScopeForTest(TEXT("chanA"));
		TArray<uint8, TAlignedHeapAllocator<16>> Payload;
		TArray<float> Scales;
		BankView View;
		TArray<int32> SourceIndices;
		const bool bBuilt = Inspector->BuildAnalysisSampleForTest(Inspector->GetPrimarySource(),
			Count, Payload, Scales, View, SourceIndices);
		TestTrue(TEXT("P1 sub-case B: chanA resolves against the archive after a zero-channel "
			"asset selection -- the guard's Num()==0 condition treats this the same as no "
			"selection at all"), bBuilt);
		if (bBuilt)
		{
			TestEqual(TEXT("P1 sub-case B: dims equal chanA's own length (4) -- the archive's "
				"channels populated the combo"), View.dims, 4);
		}
	}

	return true;
}

// ===========================================================================
// Item 3 (round-2, this round's own settlement question): does
// OpenSecondScratchArchiveFromBytes() leave any source-dependent UI state stale by NOT
// resyncing the channel sliders/projection scope or clearing ProjectedPoints/ProjectionStatus,
// the way the primary slot's OpenScratchArchiveFromBytes() does? Determined at source
// (SSuperFAISSBankInspector.cpp) and confirmed here by execution: NO.
// ResyncChannelSlidersToPrimarySource(), ResetProjectionScope(), BuildProjection's
// channel/scope resolution, and RunQuery's Args.Channels.Add all read GetPrimarySource()
// exclusively, never GetSecondSource() -- so nothing channel- or projection-related can go
// stale from a second-slot open no matter what it does or does not reset. Everything that IS
// second-source-dependent (MatchPairResults, CorrespondenceStatus, NoveltyResult, the
// Structure* fields) is already unconditionally cleared by InvalidateAnalysisCaches(), which
// OpenSecondScratchArchiveFromBytes() already calls. There
// is no stale-state gap on the second slot; the omission the finding's literal wording
// ("primary") named is correct by design, not a scoped-out fix. No production change made
// either way -- this cell pins the design fact as an executable regression guard.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSSecondSlotArchiveOpenChannelStateUnchangedTest,
	"SuperFAISS.D.SecondSlotArchiveOpenChannelStateUnchanged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSSecondSlotArchiveOpenChannelStateUnchangedTest::RunTest(const FString& Parameters)
{
	using namespace superfaiss;
	constexpr int32 Count = 6;
	constexpr int32 Dims = 8;

	// Primary: an asset carrying "chanA"/"chanB" -- populates the combo.
	USuperFAISSVectorBank* PrimaryAsset = MakeGapClosureAsset(*this, Count, Dims,
		{TEXT("chanA"), TEXT("chanB")}, {0, 4}, {4, 4}, 0xB001ull);
	if (PrimaryAsset == nullptr) { return true; }

	// Second slot: a DIFFERENT channel geometry ("chanZ" only) -- if the second-slot open
	// wrongly repopulated the combo from the SECOND source, this would be immediately visible as
	// "chanA" no longer resolving to the channel's own dims.
	const TArray<FName> SecondNames = {TEXT("chanZ")};
	const TArray<int32> SecondOffsets = {0};
	const TArray<int32> SecondLengths = {4};
	TArray<uint8> SecondBytes;
	if (!MakeGapClosureArchiveBytes(*this, Count, Dims, SecondNames, SecondOffsets, SecondLengths,
		0xB002ull, SecondBytes))
	{
		return true;
	}

	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	Inspector->SetBankForTest(PrimaryAsset);
	Inspector->SetAnalysisScopeForTest(TEXT("chanA"));

	TestTrue(TEXT("second archive opens"),
		Inspector->OpenSecondScratchArchiveFromBytes(SecondBytes, TEXT("second-slot-archive.bin")));

	// Channel state: chanA still resolves against the PRIMARY source at its own dims (4) -- the
	// second-slot open did not touch RepopulateChannelState() or the combo's content.
	TArray<uint8, TAlignedHeapAllocator<16>> Payload;
	TArray<float> Scales;
	BankView View;
	TArray<int32> SourceIndices;
	const bool bBuilt = Inspector->BuildAnalysisSampleForTest(Inspector->GetPrimarySource(),
		Count, Payload, Scales, View, SourceIndices);
	TestTrue(TEXT("chanA still resolves against the primary source after the second-slot open"),
		bBuilt);
	if (bBuilt)
	{
		TestEqual(TEXT("dims still equal chanA's own length (4) -- the second-slot open did not "
			"repopulate or otherwise disturb the primary-driven channel state"), View.dims, 4);
	}

	return true;
}

// ===========================================================================
// T-1100 (2026-07-26) -- outside-review round-2 finding 1 (re-filed from a colliding T-1002
// -- see board row T-1101): before this fix,
// OpenScratchArchiveFromBytes()'s conditional RepopulateChannelState() rebuild skipped
// whenever ChannelSliderNames was already non-empty -- P1's own fix, proven correct for the
// fresh/asset preconditions by ArchiveOpenChannelStateFresh and
// ArchiveOpenChannelStateAfterUnrelatedAsset above. Neither of those two cells opens a SECOND
// channel-carrying PRIMARY ARCHIVE after a first one, which is the sequence the finding names:
// the guard's skip left ChannelSliderNames holding the FIRST archive's channel names on
// screen, while RunQuery's Args.Channels.Add resolved each slider's
// WEIGHT against Source.GetChannelName(C) -- the CURRENT primary source, i.e. the second
// archive -- purely by array position C. When the two archives carried the SAME channel count
// and DIFFERENT names, nothing tied the displayed label to the channel the weight was
// actually applied to: the query was well-formed and the label was true-looking, but the label
// named the wrong archive's channel.
//
// The fix (SSuperFAISSBankInspector.cpp, the OpenScratchArchiveFromBytes resync guard): the
// guard now also repopulates whenever the
// source that populated the current combo was itself a primary archive
// (bPreviousPrimaryWasArchive, captured before the new archive's bytes are committed), so a
// SECOND (or later) primary-archive open resyncs the combo to the new archive's own channel
// names. This closes the invariant that
// must hold at query time (the code review's own ceiling statement, not a remedy it prescribed):
// ChannelSliderNames[C] == GetPrimarySource().GetChannelName(C) for every C. This cell asserts
// exactly that invariant, directly, rather than exercising RunQuery itself: RunQuery is
// private with no test seam to call it or to observe the FSuperFAISSHit/FSuperFAISSQueryArgs
// it builds, and ChannelSliderNames is a private field with no accessor (the test author's own
// writable scope for this cell is test code only; a production accessor is not added here per the
// brief). The label a slider actually RENDERS is public information exactly once already in
// this file -- the M-2 cell above (CorrespondenceDocumentationUrl assertion (d)) walks the
// built Slate tree via the engine's own SWidget::GetChildren()/GetTypeAsString() to prove a
// hyperlink widget is reachable in the layout, with no test-support seam and no production
// change; the same idiom is used here to read a channel slider's rendered STextBlock text
// instead of merely checking for a widget's presence. The label format is fixed and unique to
// this row (RebuildChannelSliders(), SSuperFAISSBankInspector.cpp: Text_Lambda "%s  %.2f" --
// name, two literal spaces, the weight to two decimals), so a text-block string matching that
// shape cannot be confused with any other label this widget renders.
// ===========================================================================

namespace
{
	// Walks the built Slate tree from Root and returns every channel-slider label's NAME
	// portion, in the order the rows were added (RebuildChannelSliders() adds one slot per
	// channel index C in increasing order, so a pre-order DFS over GetChildren() visits them
	// in that same order) -- the name portion is captured by matching the row's own fixed
	// render format ("%s  %.2f") rather than assuming which STextBlock in the tree is the
	// slider's, so a label whose weight has never been touched (still the default "1.00") is
	// found exactly the same way a moved one would be.
	TArray<FString> CollectChannelSliderLabels(TSharedRef<SWidget> Root)
	{
		TArray<FString> Labels;
		const FRegexPattern LabelPattern(TEXT("^(.*)  [0-9]+\\.[0-9]{2}$"));
		TFunction<void(TSharedRef<SWidget>)> Walk = [&Labels, &LabelPattern, &Walk](TSharedRef<SWidget> Widget)
		{
			if (Widget->GetTypeAsString() == TEXT("STextBlock"))
			{
				const FString Text = StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString();
				FRegexMatcher Matcher(LabelPattern, Text);
				if (Matcher.FindNext())
				{
					Labels.Add(Matcher.GetCaptureGroup(1));
				}
			}
			FChildren* Children = Widget->GetChildren();
			for (int32 i = 0; i < Children->Num(); ++i)
			{
				Walk(Children->GetChildAt(i));
			}
		};
		Walk(Root);
		return Labels;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSArchiveOpenChannelSliderLabelIdentityTest,
	"SuperFAISS.D.ArchiveOpenChannelSliderLabelIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSArchiveOpenChannelSliderLabelIdentityTest::RunTest(const FString& Parameters)
{
	using namespace superfaiss;
	constexpr int32 Count = 6;
	constexpr int32 Dims = 8;

	// Archive A: chanA/chanB.
	const TArray<FName> NamesA = {TEXT("chanA"), TEXT("chanB")};
	const TArray<int32> OffsetsA = {0, 4};
	const TArray<int32> LengthsA = {4, 4};
	TArray<uint8> BytesA;
	if (!MakeGapClosureArchiveBytes(*this, Count, Dims, NamesA, OffsetsA, LengthsA, 0xF010ull, BytesA))
	{
		return true;
	}

	// Archive B: chanY/chanZ -- the SAME channel COUNT as archive A (2), DISJOINT names. This
	// is the exact shape the finding names: equal counts is what makes ChannelWeights.Num() ==
	// ChannelCount still hold at RunQuery, so bChannels stays true and the stale-labelled query
	// is issued rather than silently degrading to an unweighted whole-row query.
	const TArray<FName> NamesB = {TEXT("chanY"), TEXT("chanZ")};
	const TArray<int32> OffsetsB = {0, 4};
	const TArray<int32> LengthsB = {4, 4};
	TArray<uint8> BytesB;
	if (!MakeGapClosureArchiveBytes(*this, Count, Dims, NamesB, OffsetsB, LengthsB, 0xF011ull, BytesB))
	{
		return true;
	}

	// Fresh, deterministic no-selection state -- this suite's own established idiom
	// (ArchiveOpenChannelStateFresh above; SuperFAISSInspectorPanelTests.cpp:447).
	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	Inspector->SetBankForTest(nullptr);

	TestTrue(TEXT("(setup) archive A opens on the primary slot"),
		Inspector->OpenScratchArchiveFromBytes(BytesA, TEXT("label-identity-archive-a.bin")));

	// (setup) Sanity control, BEFORE the second archive opens: the label-reading mechanism
	// itself (the Slate-tree walk plus the fixed-format regex) is sound, proven by the
	// invariant holding on the ONLY archive opened so far -- this is what the P1 guard already
	// gets right (SuperFAISS.D.ArchiveOpenChannelStateFresh's own cell). A failure here would
	// mean the harness is broken, not that the finding reproduced -- distinguished from the
	// real assertion below by a separate, earlier TestEqual pass/fail.
	{
		const TArray<FString> LabelsAfterA = CollectChannelSliderLabels(Inspector);
		const FSuperFAISSInspectionSource SourceAfterA = Inspector->GetPrimarySource();
		TestEqual(TEXT("(setup) two channel-slider labels render after archive A opens alone"),
			LabelsAfterA.Num(), 2);
		if (LabelsAfterA.Num() == 2)
		{
			for (int32 C = 0; C < 2; ++C)
			{
				TestEqual(FString::Printf(TEXT("(setup) slider label %d equals archive A's own "
					"channel name before a second archive is ever opened -- confirms the label-"
					"reading mechanism reads real, current state, not a stale or default string"),
					C),
					LabelsAfterA[C], SourceAfterA.GetChannelName(C).ToString());
			}
		}
	}

	// The reachable sequence: open archive B on the SAME (primary) slot, superseding archive A.
	// Before T-1100's fix, OpenScratchArchiveFromBytes() left ChannelSliderNames untouched
	// whenever it was already non-empty, so archive B's own channel names never entered the
	// combo. At HEAD, ResyncChannelSlidersToPrimarySource() runs unconditionally on every
	// primary-source change (D-INSP-27), so this open always resyncs the sliders to archive B's
	// own channels regardless of what populated the combo before it.
	TestTrue(TEXT("archive B opens on the primary slot, superseding archive A"),
		Inspector->OpenScratchArchiveFromBytes(BytesB, TEXT("label-identity-archive-b.bin")));

	const TArray<FString> LabelsAfterB = CollectChannelSliderLabels(Inspector);
	const FSuperFAISSInspectionSource SourceAfterB = Inspector->GetPrimarySource();
	TestEqual(TEXT("(setup) two channel-slider labels still render after archive B opens -- "
		"both fixtures carry two channels, and the resync repopulates rather than clearing the "
		"combo"),
		LabelsAfterB.Num(), 2);
	if (LabelsAfterB.Num() == 2)
	{
		for (int32 C = 0; C < 2; ++C)
		{
			// THE INVARIANT (D-INSP-27 / T-1100, an outside code review's round-2 finding 1):
			// RunQuery's Args.Channels.Add({Source.GetChannelName(C), ChannelWeights[C]}) --
			// the weight this slider's label displays is bound to Source.GetChannelName(C) (the
			// CURRENT primary source, archive B) purely by position. The invariant that must
			// hold for the label to describe the query it controls is
			// ChannelSliderNames[C] == GetPrimarySource().GetChannelName(C);
			// ResyncChannelSlidersToPrimarySource()'s unconditional resync on every
			// primary-source change keeps it true, so the label reads archive B's own channel
			// name, matching GetPrimarySource()'s.
			TestEqual(FString::Printf(TEXT("T-1100 / an outside code review's round-2 finding 1 "
				"(ResyncChannelSlidersToPrimarySource -> RunQuery's Args.Channels.Add): the "
				"channel-slider label rendered at index %d "
				"must equal GetPrimarySource().GetChannelName(%d), since RunQuery binds that "
				"slider's weight to the channel GetPrimarySource() reports at this position"), C, C),
				LabelsAfterB[C], SourceAfterB.GetChannelName(C).ToString());
		}
	}

	return true;
}

// ===========================================================================
// T-1121 (2026-07-26) -- a confirmation review of the T-1100 remedy, finding 2: at the time of
// that finding, the channel-slider resync shared the same guard as the projection-scope combo
// (`ChannelSliderNames.Num() == 0 || bPreviousPrimaryWasArchive`), which closed the
// archive-to-archive form of the mislabelled-weight failure (the cell immediately above) but
// explicitly excluded the FIRST primary-archive open after an ASSET selection: neither half of
// the guard's condition was true there, so the resync was skipped and the combo kept the asset's
// own channel names on screen while GetPrimarySource() already resolved to the archive. D-INSP-27
// (below) decouples the two: ResyncChannelSlidersToPrimarySource() now runs unconditionally on
// every primary-source change, independent of the guard, so this sequence resyncs the labels too
// -- the guard's asset-driven exclusion (section 25.3's design note) now governs only the
// projection-scope combo, whose three GetChannelIndex call sites (`SSuperFAISSBankInspector.cpp`)
// all resolve SelectedProjectionScope, never a channel weight; RunQuery's Args.Channels.Add binds
// each slider's weight to Source.GetChannelName(C) purely by array position C, which is why the
// scope combo can stay asset-driven while the weight sliders cannot.
//
// This pins the same invariant SuperFAISS.D.ArchiveOpenChannelSliderLabelIdentity (above) pins --
// ChannelSliderNames[C] == GetPrimarySource().GetChannelName(C) -- for the sequence that cell
// cannot see (it never puts an asset before the first archive open).
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSAssetThenArchiveChannelSliderLabelIdentityTest,
	"SuperFAISS.D.AssetThenArchiveChannelSliderLabelIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSAssetThenArchiveChannelSliderLabelIdentityTest::RunTest(const FString& Parameters)
{
	using namespace superfaiss;
	constexpr int32 Count = 6;
	constexpr int32 Dims = 8;

	// The asset: two channels, disjoint from the archive's below.
	const TArray<FName> AssetNames = {TEXT("assetC1"), TEXT("assetC2")};
	const TArray<int32> AssetOffsets = {0, 4};
	const TArray<int32> AssetLengths = {4, 4};
	USuperFAISSVectorBank* Asset = MakeGapClosureAsset(
		*this, Count, Dims, AssetNames, AssetOffsets, AssetLengths, 0xF012ull);
	if (Asset == nullptr) { return true; }

	// The archive: the SAME channel COUNT (2), DISJOINT names -- the exact shape the finding
	// named: equal counts is what kept RunQuery's `bChannels` guard true rather than degrading to
	// an unweighted query, which is what made the mislabel (rather than a silent degrade)
	// reproducible before D-INSP-27's unconditional resync closed it.
	const TArray<FName> ArchiveNames = {TEXT("archD1"), TEXT("archD2")};
	const TArray<int32> ArchiveOffsets = {0, 4};
	const TArray<int32> ArchiveLengths = {4, 4};
	TArray<uint8> ArchiveBytes;
	if (!MakeGapClosureArchiveBytes(
		*this, Count, Dims, ArchiveNames, ArchiveOffsets, ArchiveLengths, 0xF013ull, ArchiveBytes))
	{
		return true;
	}

	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	Inspector->SetBankForTest(Asset);

	// (setup) Sanity control, BEFORE the archive ever opens: the asset's own channel labels are
	// exactly its own channel names. This isolates the label-reading mechanism from the finding
	// under test -- a failure here means the harness is broken, not that the finding reproduced,
	// and it is asserted separately from the real assertion below by its own TestEqual calls.
	{
		const TArray<FString> LabelsAfterAsset = CollectChannelSliderLabels(Inspector);
		const FSuperFAISSInspectionSource SourceAfterAsset = Inspector->GetPrimarySource();
		TestEqual(TEXT("(setup) two channel-slider labels render after the asset is selected alone"),
			LabelsAfterAsset.Num(), 2);
		if (LabelsAfterAsset.Num() == 2)
		{
			for (int32 C = 0; C < 2; ++C)
			{
				TestEqual(FString::Printf(TEXT("(setup) slider label %d equals the asset's own "
					"channel name before any archive is ever opened -- confirms the label-reading "
					"mechanism reads real, current state, not a stale or default string"), C),
					LabelsAfterAsset[C], SourceAfterAsset.GetChannelName(C).ToString());
			}
		}
	}

	// The reachable sequence the finding names: the FIRST primary-archive open after an asset
	// selection. Before D-INSP-27, the channel-slider resync shared the projection-scope guard
	// (`ChannelSliderNames.Num() == 0 || bPreviousPrimaryWasArchive`) -- both false here (the
	// asset already populated the combo, and the source that populated it was NOT an archive) --
	// so the resync was explicitly skipped and the combo kept the asset's own channel names on
	// screen. At HEAD, ResyncChannelSlidersToPrimarySource() runs unconditionally on every
	// primary-source change, so this open resyncs the sliders to the archive's own channels
	// regardless of the guard.
	TestTrue(TEXT("archive opens on the primary slot, superseding the asset"),
		Inspector->OpenScratchArchiveFromBytes(ArchiveBytes, TEXT("asset-then-archive.bin")));

	const TArray<FString> LabelsAfterArchive = CollectChannelSliderLabels(Inspector);
	const FSuperFAISSInspectionSource SourceAfterArchive = Inspector->GetPrimarySource();
	TestEqual(TEXT("(setup) two channel-slider labels still render after the archive opens -- "
		"both fixtures carry two channels, and the resync repopulates rather than clearing the "
		"combo"),
		LabelsAfterArchive.Num(), 2);
	if (LabelsAfterArchive.Num() == 2)
	{
		for (int32 C = 0; C < 2; ++C)
		{
			// THE INVARIANT (D-INSP-27 closes a confirmation review's finding 2;
			// ResyncChannelSlidersToPrimarySource -> RunQuery's Args.Channels.Add): RunQuery
			// issues Args.Channels.Add({Source.GetChannelName(C), ChannelWeights[C]}) -- the
			// weight this slider's label displays is bound to Source.GetChannelName(C) (the
			// CURRENT primary source, the archive) purely by position. The invariant that must
			// hold for the label to describe the query it controls is
			// ChannelSliderNames[C] == GetPrimarySource().GetChannelName(C);
			// ResyncChannelSlidersToPrimarySource()'s unconditional resync on every
			// primary-source change keeps it true for this sequence, independent of the
			// projection-scope guard. On this archive-open path, a failure here starts at
			// ResyncChannelSlidersToPrimarySource() (called directly from
			// OpenScratchArchiveFromBytes(), not through RepopulateChannelState()) or at the
			// projection-scope guard, which govern this array and the scope combo
			// respectively; on the OnBankSelected() path, RepopulateChannelState() calls the
			// same ResyncChannelSlidersToPrimarySource() and does govern this array.
			TestEqual(FString::Printf(TEXT("A confirmation review's finding 2 (asset-to-archive "
				"form, ResyncChannelSlidersToPrimarySource -> RunQuery's Args.Channels.Add): the "
				"channel-slider label rendered "
				"at index %d must equal GetPrimarySource().GetChannelName(%d), since RunQuery "
				"binds that slider's weight to the channel GetPrimarySource() reports at this "
				"position"), C, C),
				LabelsAfterArchive[C], SourceAfterArchive.GetChannelName(C).ToString());
		}
	}

	return true;
}

// ===========================================================================
// D-INSP-27 (2026-07-26): channel-slider weights reset to their 1.0 default on EVERY
// primary-source change, asset or archive -- V32-G2 (`SuperFAISS_V2_Plan.md:2163`) and the
// shipped class tooltip (`SuperFAISSInspectorSettings.h:13-16`) both already said so; the
// by-name carryover `RepopulateChannelState()` gained under T-1100 contradicts both and is
// ruled removed rather than scoped. Both cells below were blocked until
// `SetChannelWeightForTest` (T-1123) existed -- with no writer of `ChannelWeights` reachable
// from test code, "the weight reads 1.0 after a change" was indistinguishable from a value
// that had simply never moved (section 11 above). The seam is now built and reproduces the
// Slate slider's own guard/assignment against the same array, so a test that drives it
// exercises the exact state `RunQuery` reads.
//
// Both cells share the local helper below (a new file-local addition, no production change,
// same convention as `CollectChannelSliderLabels` above): the existing helper's regex discards
// the weight half of the rendered `"%s  %.2f"` label, keeping only the name, so it cannot observe
// the axis these two cells exist to pin. This one keeps both captures.
// ===========================================================================

namespace
{
	// Same Slate-tree walk and rendered-label format `CollectChannelSliderLabels` already
	// establishes as this suite's read surface for private slider state (section 10 above; no
	// public accessor exists for `ChannelWeights`, confirmed at source in section 11) -- extended
	// to keep the weight capture group instead of discarding it, since these two cells assert on
	// the weight, not the name.
	TArray<TPair<FString, float>> CollectChannelSliderNameWeightPairs(TSharedRef<SWidget> Root)
	{
		TArray<TPair<FString, float>> Pairs;
		const FRegexPattern LabelPattern(TEXT("^(.*)  ([0-9]+\\.[0-9]{2})$"));
		TFunction<void(TSharedRef<SWidget>)> Walk = [&Pairs, &LabelPattern, &Walk](TSharedRef<SWidget> Widget)
		{
			if (Widget->GetTypeAsString() == TEXT("STextBlock"))
			{
				const FString Text = StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString();
				FRegexMatcher Matcher(LabelPattern, Text);
				if (Matcher.FindNext())
				{
					Pairs.Add(TPair<FString, float>(Matcher.GetCaptureGroup(1),
						FCString::Atof(*Matcher.GetCaptureGroup(2))));
				}
			}
			FChildren* Children = Widget->GetChildren();
			for (int32 i = 0; i < Children->Num(); ++i)
			{
				Walk(Children->GetChildAt(i));
			}
		};
		Walk(Root);
		return Pairs;
	}
}

// ===========================================================================
// Cell A -- D-INSP-27's reset claim on the primary-BANK-change path (V32-G2,
// `SuperFAISS_V2_Plan.md:2163`; the tooltip, `SuperFAISSInspectorSettings.h:13-16`). Two assets
// carrying the SAME channel name ("text") is deliberate, not incidental: it is the one shape
// under which `RepopulateChannelState()`'s by-name carryover (T-1100) finds a name match and
// PRESERVES the outgoing weight across the swap. A cell built with two DIFFERENT channel names
// would pass identically whether the carryover is present or removed -- the name lookup would
// already miss and reset to default either way, proving nothing about the carryover's presence.
// Same name is what made this cell fail red under the carryover, and it passed once D-INSP-27's
// removal landed.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSPrimaryBankChangeResetsChannelWeightsTest,
	"SuperFAISS.D.PrimaryBankChangeResetsChannelWeights",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSPrimaryBankChangeResetsChannelWeightsTest::RunTest(const FString& Parameters)
{
	constexpr int32 Count = 6;
	constexpr int32 Dims = 8;
	const TArray<FName> Names = {TEXT("text")};
	const TArray<int32> Offsets = {0};
	const TArray<int32> Lengths = {4};

	USuperFAISSVectorBank* AssetOne = MakeGapClosureAsset(*this, Count, Dims, Names, Offsets, Lengths, 0xC001ull);
	USuperFAISSVectorBank* AssetTwo = MakeGapClosureAsset(*this, Count, Dims, Names, Offsets, Lengths, 0xC002ull);
	if (AssetOne == nullptr || AssetTwo == nullptr) { return true; }

	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	Inspector->SetBankForTest(AssetOne);

	// (setup) sanity control 1, before the seam ever runs: isolates the harness itself (asset
	// selection, the render, the read helper) from the seam and from the cell under test. A
	// failure here means the fixture or the read surface is broken, not that D-INSP-27 held.
	{
		const TArray<TPair<FString, float>> Initial = CollectChannelSliderNameWeightPairs(Inspector);
		TestEqual(TEXT("(setup) one channel slider renders after AssetOne is selected"), Initial.Num(), 1);
		if (Initial.Num() == 1)
		{
			TestEqual(TEXT("(setup) 'text' names the rendered slider"), Initial[0].Key, FString(TEXT("text")));
			TestTrue(TEXT("(setup) the slider starts at its 1.0 default"),
				FMath::IsNearlyEqual(Initial[0].Value, 1.0f, 1e-3f));
		}
	}

	// Drive the weight away from default THROUGH THE SEAM. The seam reproduces the exact guard
	// and assignment the Slate slider's OnValueChanged lambda performs against the same
	// ChannelWeights array, so for this in-range value (0.0f, within the slider's own [0, 2])
	// this is the real query-binding path a user's slider drag drives, not a substitute.
	Inspector->SetChannelWeightForTest(0, 0.0f);

	// (setup) sanity control 2, before the bank change: confirms the seam actually moved the
	// rendered weight to the non-default value this cell needs. A failure here means the seam or
	// the harness is broken, not that the reset claim held or failed.
	{
		const TArray<TPair<FString, float>> AfterSeam = CollectChannelSliderNameWeightPairs(Inspector);
		TestEqual(TEXT("(setup) one channel slider still renders after the seam call"), AfterSeam.Num(), 1);
		if (AfterSeam.Num() == 1)
		{
			TestTrue(TEXT("(setup) the seam moved 'text''s rendered weight to the non-default 0.0 "
				"this cell set"), FMath::IsNearlyEqual(AfterSeam[0].Value, 0.0f, 1e-3f));
		}
	}

	// The primary-BANK change the cell exists to prove: selecting AssetTwo (SAME channel name
	// "text", different underlying data) drives OnBankSelected() -> RepopulateChannelState()
	// unconditionally.
	Inspector->SetBankForTest(AssetTwo);

	const TArray<TPair<FString, float>> AfterSwap = CollectChannelSliderNameWeightPairs(Inspector);
	TestEqual(TEXT("D-INSP-27: one channel slider renders for AssetTwo's own 'text' channel"),
		AfterSwap.Num(), 1);
	if (AfterSwap.Num() == 1)
	{
		TestEqual(TEXT("(setup) the surviving slider is still named 'text' -- confirms this is the "
			"name-collision path the carryover keys on, not a coincidental empty combo"),
			AfterSwap[0].Key, FString(TEXT("text")));
		TestTrue(TEXT("D-INSP-27 (V32-G2, SuperFAISSInspectorSettings.h:13-16): every channel weight "
			"resets to its 1.0 default on a primary-BANK change, even when the outgoing and "
			"incoming banks share a channel name. A regression that reintroduces "
			"RepopulateChannelState()'s by-name carryover (removed under D-INSP-27, previously "
			"added under T-1100) would preserve the pre-change 0.0 weight across this exact "
			"same-name swap instead of resetting it"),
			FMath::IsNearlyEqual(AfterSwap[0].Value, 1.0f, 1e-3f));
	}

	return true;
}

// ===========================================================================
// Cell C -- D-INSP-27's reset claim on the exact path T-1100's by-name carryover was WRITTEN
// for: a second (or later) primary-ARCHIVE open resyncing over a prior primary archive
// (`bPreviousPrimaryWasArchive`, SSuperFAISSBankInspector.cpp:1141,1185). Archive A and archive B
// share the SAME channel name ("text") for the identical reason Cell A's two assets do -- the
// carryover's name lookup only has something to preserve when the outgoing and incoming sources
// agree on a channel name; a disjoint-name pair (as SuperFAISS.D.ArchiveOpenChannelSliderLabelIdentity
// above uses, to pin a different invariant) would reset to default under the carryover exactly as
// it would with the carryover removed, proving nothing about D-INSP-27's own claim.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSArchiveResyncResetsChannelWeightsTest,
	"SuperFAISS.D.ArchiveResyncResetsChannelWeights",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSArchiveResyncResetsChannelWeightsTest::RunTest(const FString& Parameters)
{
	constexpr int32 Count = 6;
	constexpr int32 Dims = 8;
	const TArray<FName> Names = {TEXT("text")};
	const TArray<int32> Offsets = {0};
	const TArray<int32> Lengths = {4};

	TArray<uint8> BytesA;
	if (!MakeGapClosureArchiveBytes(*this, Count, Dims, Names, Offsets, Lengths, 0xC011ull, BytesA))
	{
		return true;
	}
	TArray<uint8> BytesB;
	if (!MakeGapClosureArchiveBytes(*this, Count, Dims, Names, Offsets, Lengths, 0xC012ull, BytesB))
	{
		return true;
	}

	// Fresh, deterministic no-selection state -- this suite's own established idiom
	// (ArchiveOpenChannelStateFresh above; SuperFAISSInspectorPanelTests.cpp:447), so archive A's
	// own open below is unambiguously "no source has ever populated the combo", not an artifact
	// of whatever RefreshBankList() auto-selected from the real asset registry.
	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	Inspector->SetBankForTest(nullptr);

	TestTrue(TEXT("(setup) archive A opens on the primary slot"),
		Inspector->OpenScratchArchiveFromBytes(BytesA, TEXT("resync-archive-a.bin")));

	// (setup) sanity control 1, before the seam ever runs: isolates the harness (the archive
	// fixture, the fresh-open path, the render, the read helper) from the seam and from the cell
	// under test.
	{
		const TArray<TPair<FString, float>> Initial = CollectChannelSliderNameWeightPairs(Inspector);
		TestEqual(TEXT("(setup) one channel slider renders after archive A opens alone"), Initial.Num(), 1);
		if (Initial.Num() == 1)
		{
			TestEqual(TEXT("(setup) 'text' names the rendered slider"), Initial[0].Key, FString(TEXT("text")));
			TestTrue(TEXT("(setup) the slider starts at its 1.0 default"),
				FMath::IsNearlyEqual(Initial[0].Value, 1.0f, 1e-3f));
		}
	}

	Inspector->SetChannelWeightForTest(0, 0.0f);

	// (setup) sanity control 2, before the archive resync: confirms the seam actually moved the
	// rendered weight to the non-default value this cell needs.
	{
		const TArray<TPair<FString, float>> AfterSeam = CollectChannelSliderNameWeightPairs(Inspector);
		TestEqual(TEXT("(setup) one channel slider still renders after the seam call"), AfterSeam.Num(), 1);
		if (AfterSeam.Num() == 1)
		{
			TestTrue(TEXT("(setup) the seam moved 'text''s rendered weight to the non-default 0.0 "
				"this cell set"), FMath::IsNearlyEqual(AfterSeam[0].Value, 0.0f, 1e-3f));
		}
	}

	// The archive RESYNC the cell exists to prove: opening archive B on the SAME (primary) slot
	// while the current primary source is ALREADY an archive (archive A) makes
	// bPreviousPrimaryWasArchive true (SSuperFAISSBankInspector.cpp:1141). At HEAD,
	// ResyncChannelSlidersToPrimarySource() resyncs the weight sliders unconditionally on this
	// path regardless of that flag or of ChannelSliderNames.Num() -- this is the exact sequence
	// T-1100's own commit message described as the (now-removed) carryover's target ("archive
	// #1's... state... a user's slider position is lost only when the name it belonged to is
	// gone").
	TestTrue(TEXT("archive B opens on the primary slot, resyncing over archive A"),
		Inspector->OpenScratchArchiveFromBytes(BytesB, TEXT("resync-archive-b.bin")));

	const TArray<TPair<FString, float>> AfterResync = CollectChannelSliderNameWeightPairs(Inspector);
	TestEqual(TEXT("D-INSP-27: one channel slider renders for archive B's own 'text' channel"),
		AfterResync.Num(), 1);
	if (AfterResync.Num() == 1)
	{
		TestEqual(TEXT("(setup) the surviving slider is still named 'text' -- confirms this is the "
			"name-collision path the carryover keys on, not a coincidental empty combo"),
			AfterResync[0].Key, FString(TEXT("text")));
		TestTrue(TEXT("D-INSP-27 (V32-G2, SuperFAISSInspectorSettings.h:13-16): every channel weight "
			"resets to its 1.0 default on a primary-source change to an archive too, including the "
			"exact archive-resync path T-1100's own carryover was written for. A regression that "
			"reintroduces RepopulateChannelState()'s by-name carryover would preserve the "
			"pre-resync 0.0 weight across this same-name swap instead of resetting it"),
			FMath::IsNearlyEqual(AfterResync[0].Value, 1.0f, 1e-3f));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
