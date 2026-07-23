// S-INSP-3.3.1 (SuperFAISS For Unreal 3.3.1, the correctness release). Gate 1a/1b: the
// independent oracle (hand-authored tutorial-bank geometry, Fixtures/TutorialBankGeometry.csv
// + SuperFAISSTutorialBankFixture.h) and the red suite it gates, realizing the plan's
// Coverage Model (Claude/Plans/SuperFAISSUnreal_3.3.1_Plan.md section 6) dims 4, 6, 8, 10, 11
// for the oracle-gated ticket set {SF34-003, SF34-004, SF34-005, SF34-006}. Every expected
// value below is derived from the sidecar's own geometry rule (cosine = ([ChanADir match] +
// [ChanBDir match]) / 2), NOT from reading SSuperFAISSBankInspector.cpp -- the independent-
// oracle law (StandardsDocument.md section 4). Authored RED, before any of the four tickets'
// fixes land: SF34-003/004/005 are read against the CURRENT shipped 3.3.0 behavior (asset-only
// Novelty, sample-position-not-source-index archive labels, outright archive-channel-scope
// rejection); SF34-006 is red because CslsMarginThreshold has no plan-pinned default.
//
// See Claude/Curie/superfaiss-3.3.1-test-design-2026-07-22.md for the full per-cell
// derivation, the routed Coverage-Model gap (no test seam exists for the rendered
// tree/tooltip label string a pruned archive's row draws — SSuperFAISSBankInspector.h's
// RebuildStructureClusterList/GetScatterPointLabel are private with no public accessor), and
// which cells are oracle-gated vs guard-vitality vs parity-matrix.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include <limits>

#include "Fixtures/SuperFAISSTutorialBankFixture.h"
#include "SSuperFAISSBankInspector.h"
#include "SuperFAISSInspectorSettings.h"
#include "superfaiss/superfaiss.h"

using namespace SuperFAISSTutorialBank;

namespace
{
	// RAII-ish snapshot/restore of the settings CDO's CslsMarginThreshold and MatchK, so a
	// test that pins them for its own derivation never leaks into a later test in the same
	// process (the InspectorSettingsPersistenceTest precedent: this suite never leaves a
	// live project's editor-per-user config polluted by a test run).
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

	// Opens the named tutorial bank as an ARCHIVE source on the given slot (primary or
	// second), with RowsToTombstone Remove()'d before Save -- the real, shipped
	// OpenScratchArchiveFromBytes/OpenSecondScratchArchiveFromBytes control-flow, exactly
	// the production seam a user's "Open scratch archive..." action drives. Returns false
	// (and fails the test) if the bake or the open itself fails.
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
}

// ===========================================================================
// SF34-005 (Coverage Model dim 4/8/11b) -- channel-scoped archive sampling. TODAY:
// BuildAnalysisSample(Source, ...) rejects a non-"(whole row)" scope outright for an
// Archive-kind source (SSuperFAISSBankInspector.cpp:1743, inside the source-kind overload
// beginning at 1714) -- the acceptance criterion is that this becomes a SUPPORTED path with
// asset/archive answer-equivalence, not merely "does not crash". This is also the guard-
// vitality cell (dim 11b): the FORMER outright rejection must be genuinely replaced (the test
// asserts the call SUCCEEDS today-red, not merely that some rejection string changed).
//
// Oracle: Primary row 11 (tag MIX-A, ChanADir=0, ChanBDir=1) chanA-scoped -- the sidecar's own
// closed-form rule means the chanA-scoped, renormalized-to-unit-norm slice of ANY row is
// exactly the one-hot unit vector on ChanADir (renormalization is unit-norm-preserving on an
// already-single-nonzero-element slice: the slice's only nonzero entry is 10.0 on element
// ChanADir, so its renormalized form is exactly 1.0 on that element, 0 elsewhere, regardless
// of the row's chanB tag). Query()'ing that chanA-scoped view with the exact one-hot unit
// query on ChanADir=0 must return cosine similarity 1.0 for every row whose ChanADir==0 (rows
// 0,1,2,3,11,15,16,20 in Primary) and 0.0 for every other row -- and the SAME must hold on the
// ARCHIVE source, byte-for-byte, because the geometry baked into the archive is identical to
// the asset's.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSTutorialArchiveChannelScopeParityTest,
	"SuperFAISS.D.TutorialArchiveChannelScopeParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSTutorialArchiveChannelScopeParityTest::RunTest(const FString& Parameters)
{
	using namespace superfaiss;

	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);

	// The asset side: BakeAsAsset + SetBankForTest + SetAnalysisScopeForTest, exactly the
	// existing suite's own idiom (SuperFAISSInspectorPanelTests.cpp's
	// InspectorChannelCosineSliceRenormalization test), so this cell's asset leg is proven by
	// the SAME construction the archive leg is compared against.
	USuperFAISSVectorBank* AssetBank = BakeAsAsset(*this, TEXT("Primary"));
	if (AssetBank == nullptr) { return true; }
	Inspector->SetBankForTest(AssetBank);
	Inspector->SetAnalysisScopeForTest(TEXT("chanA"));

	FSuperFAISSInspectionSource AssetSource;
	AssetSource.Kind = FSuperFAISSInspectionSource::EKind::Asset;
	AssetSource.Asset = AssetBank;

	TArray<uint8, TAlignedHeapAllocator<16>> AssetPayload;
	TArray<float> AssetScales;
	BankView AssetView;
	TArray<int32> AssetSourceIndices;
	const bool bAssetBuilt = Inspector->BuildAnalysisSampleForTest(
		AssetSource, AssetBank->Count, AssetPayload, AssetScales, AssetView, AssetSourceIndices);
	TestTrue(TEXT("(setup) asset chanA-scoped sample builds"), bAssetBuilt);
	if (!bAssetBuilt) { return true; }

	alignas(16) float QueryBuf[4] = {1.0f, 0.0f, 0.0f, 0.0f}; // the exact chanA=0 unit direction
	Workspace AssetWs;
	Hit AssetHits[32];
	int32_t AssetHitCount = 0;
	QueryParams AssetParams;
	AssetParams.k = FMath::Min(32, AssetView.count);
	const Status AssetQueryStatus = Query(AssetView, QueryBuf, AssetParams, AssetWs, AssetHits, &AssetHitCount);
	TestEqual(TEXT("(setup) asset chanA query succeeds"),
		static_cast<int>(AssetQueryStatus), static_cast<int>(Status::Ok));

	// The golden set: every Primary row with ChanADir==0 (per the sidecar) scores 1.0; every
	// other row scores 0.0. Read off the sidecar directly: rows 0,1,2,3 (X cluster),
	// 11 (MIX-A), 15 (X5), 16 (MIX-C), 20 (MIX-G).
	TSet<int32> ExpectedChanA0 = {0, 1, 2, 3, 11, 15, 16, 20};
	auto CheckHits = [this](const Hit* Hits, int32 HitCount, const TArray<int32>& SourceIndices,
		const TSet<int32>& ExpectedOnes, const TCHAR* Label)
	{
		TSet<int32> ActualOnes;
		for (int32 i = 0; i < HitCount; ++i)
		{
			if (!SourceIndices.IsValidIndex(Hits[i].index)) { continue; }
			const int32 SourceRow = SourceIndices[Hits[i].index];
			if (FMath::Abs(Hits[i].score - 1.0f) < 1e-5f)
			{
				ActualOnes.Add(SourceRow);
			}
			else
			{
				TestTrue(FString::Printf(TEXT("%s: row %d (not ChanADir=0) scores ~0.0, got %f"),
					Label, SourceRow, Hits[i].score), FMath::Abs(Hits[i].score) < 1e-5f);
			}
		}
		// TestEqual has no TSet overload in this codebase's existing convention (TArray<int32>
		// is the proven comparable shape, e.g. InstrumentationTests.cpp's OutliersOff/On
		// comparison) -- sort both sides into arrays before comparing.
		TArray<int32> ActualSorted = ActualOnes.Array();
		TArray<int32> ExpectedSorted = ExpectedOnes.Array();
		ActualSorted.Sort();
		ExpectedSorted.Sort();
		TestEqual(FString::Printf(TEXT("%s: rows scoring 1.0 match the ChanADir=0 golden set"), Label),
			ActualSorted, ExpectedSorted);
	};
	CheckHits(AssetHits, AssetHitCount, AssetSourceIndices, ExpectedChanA0, TEXT("asset"));

	// The archive side: SAME geometry, opened via the real production seam. RED TODAY:
	// BuildAnalysisSample(Source, ...) rejects a channel scope outright for an Archive-kind
	// source (the current SelectedProjectionScope test at line 1743) -- this call is expected
	// to fail on shipped 3.3.0 and to succeed, with the SAME golden answer as the asset leg
	// above, once SF34-005 lands.
	if (!OpenTutorialArchive(*this, Inspector.Get(), TEXT("Primary"), {}, /*bSecondSlot*/ false))
	{
		return true;
	}
	Inspector->SetAnalysisScopeForTest(TEXT("chanA"));
	const FSuperFAISSInspectionSource ArchiveSource = Inspector->GetPrimarySource();
	TestEqual(TEXT("(setup) primary source resolves to Archive kind after opening"),
		static_cast<int32>(ArchiveSource.Kind), static_cast<int32>(FSuperFAISSInspectionSource::EKind::Archive));

	TArray<uint8, TAlignedHeapAllocator<16>> ArchivePayload;
	TArray<float> ArchiveScales;
	BankView ArchiveView;
	TArray<int32> ArchiveSourceIndices;
	const bool bArchiveBuilt = Inspector->BuildAnalysisSampleForTest(
		ArchiveSource, ArchiveSource.GetCount(), ArchivePayload, ArchiveScales, ArchiveView, ArchiveSourceIndices);
	TestTrue(TEXT("SF34-005 (dim 11b): channel-scoped archive sample build succeeds "
		"(the former outright rejection is genuinely replaced, not left inert)"), bArchiveBuilt);
	if (!bArchiveBuilt) { return true; }

	Workspace ArchiveWs;
	Hit ArchiveHits[32];
	int32_t ArchiveHitCount = 0;
	QueryParams ArchiveParams;
	ArchiveParams.k = FMath::Min(32, ArchiveView.count);
	const Status ArchiveQueryStatus = Query(ArchiveView, QueryBuf, ArchiveParams, ArchiveWs, ArchiveHits, &ArchiveHitCount);
	TestEqual(TEXT("archive chanA query succeeds"),
		static_cast<int>(ArchiveQueryStatus), static_cast<int>(Status::Ok));
	CheckHits(ArchiveHits, ArchiveHitCount, ArchiveSourceIndices, ExpectedChanA0, TEXT("archive"));

	// dim 4/8 parity, stated directly rather than only implied by both legs independently
	// matching the same golden set: the archive's sampled row COUNT for a full-bank,
	// channel-scoped, no-tombstone build equals the asset's (both admit every Primary row --
	// no zero-energy exclusions exist in this geometry, every row has nonzero energy on both
	// channels by construction).
	TestEqual(TEXT("dim 4/8 parity: asset and archive channel-scoped sample counts match"),
		ArchiveView.count, AssetView.count);

	return true;
}

// ===========================================================================
// SF34-005, pruned leg (dim 4/8's "full vs pruned" crossing) -- the SAME channel-scoped
// archive build, now with Primary row 15 (an extra X-cluster duplicate, ChanADir=0,
// ChanBDir=0) tombstoned. Golden set for ChanADir==0 loses row 15 and gains nothing (no other
// row is un-tombstoned): {0,1,2,3,11,16,20}. This is the dim-4/8 "generality cell" the plan
// requires asserted directly, not implied by the whole-bank cell above.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSTutorialArchiveChannelScopePrunedParityTest,
	"SuperFAISS.D.TutorialArchiveChannelScopePrunedParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSTutorialArchiveChannelScopePrunedParityTest::RunTest(const FString& Parameters)
{
	using namespace superfaiss;

	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	// A channel-carrying ASSET first, exactly the sibling (unpruned) test's own setup: the
	// projection-scope combo (ProjectionScopes/SetAnalysisScopeForTest's match set) is
	// asset-driven regardless of which source ends up primary (section 25.3's design note)
	// -- opening the archive alone, with no asset ever selected, leaves ProjectionScopes
	// empty, so SetAnalysisScopeForTest("chanA") below would silently no-op and the sample
	// would build at whole-row (16-dim) scope instead of chanA (4-dim), which is exactly
	// what a 4-float query buffer's own ValidateQuery finiteness scan over 16 dims reads as
	// garbage past the array bound. Bake-then-select first so the scope actually resolves.
	USuperFAISSVectorBank* AssetBank = BakeAsAsset(*this, TEXT("Primary"));
	if (AssetBank == nullptr) { return true; }
	Inspector->SetBankForTest(AssetBank);
	if (!OpenTutorialArchive(*this, Inspector.Get(), TEXT("Primary"), {15}, /*bSecondSlot*/ false))
	{
		return true;
	}
	Inspector->SetAnalysisScopeForTest(TEXT("chanA"));
	const FSuperFAISSInspectionSource Source = Inspector->GetPrimarySource();
	TestEqual(TEXT("(setup) live count is 21 after tombstoning row 15"), Source.GetLiveCount(), 21);

	TArray<uint8, TAlignedHeapAllocator<16>> Payload;
	TArray<float> Scales;
	BankView View;
	TArray<int32> SourceIndices;
	// Sample-scoped build: bSkipTombstonedRows=true (the default), SampleLimit = the live
	// count, so every live row is admitted.
	const bool bBuilt = Inspector->BuildAnalysisSampleForTest(
		Source, Source.GetLiveCount(), Payload, Scales, View, SourceIndices);
	TestTrue(TEXT("SF34-005: pruned, channel-scoped archive sample build succeeds"), bBuilt);
	if (!bBuilt) { return true; }
	TestFalse(TEXT("SF34-004 (dim 4, pruned leg): the tombstoned row 15 never appears in the sample"),
		SourceIndices.Contains(15));
	TestEqual(TEXT("sample count equals live count (nothing else excluded)"), View.count, 21);

	alignas(16) float QueryBuf[4] = {1.0f, 0.0f, 0.0f, 0.0f};
	Workspace Ws;
	Hit Hits[32];
	int32_t HitCount = 0;
	QueryParams Params;
	Params.k = FMath::Min(32, View.count);
	const Status QueryStatus = Query(View, QueryBuf, Params, Ws, Hits, &HitCount);
	TestEqual(TEXT("query over the pruned channel-scoped view succeeds"),
		static_cast<int>(QueryStatus), static_cast<int>(Status::Ok));

	TSet<int32> ExpectedChanA0AfterPrune = {0, 1, 2, 3, 11, 16, 20}; // row 15 dropped, no other change
	TSet<int32> ActualOnes;
	for (int32 i = 0; i < HitCount; ++i)
	{
		if (!SourceIndices.IsValidIndex(Hits[i].index)) { continue; }
		if (FMath::Abs(Hits[i].score - 1.0f) < 1e-5f)
		{
			ActualOnes.Add(SourceIndices[Hits[i].index]);
		}
	}
	{
		TArray<int32> ActualSorted = ActualOnes.Array();
		TArray<int32> ExpectedSorted = ExpectedChanA0AfterPrune.Array();
		ActualSorted.Sort();
		ExpectedSorted.Sort();
		TestEqual(TEXT("SF34-004/005 pruned-archive channel-scoped golden set"), ActualSorted, ExpectedSorted);
	}

	// The SF34-004 index-identity claim at the level a public accessor can prove today: the
	// underlying sample-position -> native-source-index mapping (SourceIndices, the same array
	// StructureSampleSourceIndices publishes after ComputeStructure/ComputeProjection) is
	// tombstone-aware and reports NATIVE indices, never shifted sample positions -- row 16
	// (immediately after the tombstoned row 15) must still resolve to native index 16, not to
	// the shifted position 15 a naive "skip nothing, just don't count the hole" implementation
	// would produce.
	const int32 PosOfRow16 = SourceIndices.IndexOfByKey(16);
	TestTrue(TEXT("SF34-004: native source index 16 is present in the pruned sample"), PosOfRow16 != INDEX_NONE);

	return true;
}

// ===========================================================================
// SF34-004 (Coverage Model dim 4/10, the routed test-seam gap -- test-design section 5) --
// archive row labels must show the original SOURCE index, never the SAMPLE position. TODAY:
// RebuildStructureClusterList's MemberLabel lambda and GetScatterPointLabel both read
// GetSelectedBank() (asset-only) rather than the source-generalized GetPrimarySource(); on an
// Archive-kind source GetSelectedBank() is nullptr, so both fall through to formatting the raw
// SAMPLE POSITION as "#<pos>" instead of resolving StructureSampleSourceIndices[pos] through
// the source and printing "#<source row>". A tombstoned prefix makes the two numbers provably
// different (the SEAM this test needs -- GetStructureMemberLabelForTest, added this round per
// the test-design's routing to Brunel -- is a WITH_DEV_AUTOMATION_TESTS-gated pass-through to
// the private ComputeStructureMemberLabel(), mirroring BuildAnalysisSampleForTest exactly).
//
// Oracle: Primary row 15 (an extra X-cluster duplicate) is tombstoned. Native source row 16 is
// therefore the first live row AFTER the tombstoned one, so its SAMPLE position (one fewer than
// its native index, because BuildAnalysisSample skips tombstoned rows and packs live rows
// contiguously) is 15, not 16 -- the two numbers are provably distinct, so a label reading the
// sample position instead of the source index is caught by direct value comparison, not by
// coincidence.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSTutorialArchiveMemberLabelSourceIndexTest,
	"SuperFAISS.D.TutorialArchiveMemberLabelSourceIndex",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSTutorialArchiveMemberLabelSourceIndexTest::RunTest(const FString& Parameters)
{
	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	if (!OpenTutorialArchive(*this, Inspector.Get(), TEXT("Primary"), {15}, /*bSecondSlot*/ false))
	{
		return true;
	}
	Inspector->ComputeStructure();

	const TArray<int32>& SourceIndices = Inspector->GetStructureSampleSourceIndices();
	const int32 SamplePosOfRow16 = SourceIndices.IndexOfByKey(16);
	TestTrue(TEXT("(setup) native source row 16 is present in the pruned sample"),
		SamplePosOfRow16 != INDEX_NONE);
	// The oracle's own load-bearing precondition: the sample position and the source index
	// must actually differ here, or this test could pass by coincidence even against the
	// buggy sample-position code path (the whole reason row 15, not row 0, is the tombstone
	// target -- it shifts every later native index down by exactly one sample position).
	TestTrue(TEXT("(setup) sample position and source index provably differ (15 != 16)"),
		SamplePosOfRow16 != 16 && SamplePosOfRow16 == 15);
	if (SamplePosOfRow16 == INDEX_NONE) { return true; }

	const FString Label = Inspector->GetStructureMemberLabelForTest(SamplePosOfRow16);
	// The archive source has no id table (GetIdForIndex is always NAME_None, the documented
	// asymmetry), so the correct label is "#16" (the SOURCE row) -- never "#15" (the sample
	// position the pre-fix code prints when GetSelectedBank() is nullptr for an archive).
	TestEqual(TEXT("SF34-004: an archive row's label shows the ORIGINAL SOURCE INDEX (#16), "
		"never the sample position (#15)"), Label, FString(TEXT("#16")));

	return true;
}

// ===========================================================================
// SF34-003 (Coverage Model dim 4/8/10) -- Novelty on either source. TODAY: ProbeNovelty()
// reads GetSelectedBank() (SSuperFAISSBankInspector.cpp:2039), which is Asset-only -- an
// opened archive with NO asset selected makes GetSelectedBank() return nullptr, so the probe
// falls straight through to "no valid bank selected" regardless of the archive's content.
// Oracle: Primary row 0 (X1, an exact duplicate exists at rows 1,2,3,15) must classify
// Duplicate; Primary row 10 (ISO-A, no exact duplicate anywhere in the bank) must NOT. Both
// facts follow directly from the sidecar: Duplicate requires another row with the SAME
// (ChanADir, ChanBDir) pair (cosine 1.0, exact identity) -- row 0's tag (0,0) repeats at
// 1,2,3,15; row 10's tag (3,2) is unique in the 22-row Primary bank (grep the sidecar: no
// other row shares ChanADir=3,ChanBDir=2).
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSTutorialArchiveNoveltyParityTest,
	"SuperFAISS.D.TutorialArchiveNoveltyParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSTutorialArchiveNoveltyParityTest::RunTest(const FString& Parameters)
{
	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);

	// Asset leg (already-real, already-shipped path): proves the oracle values themselves
	// before comparing the archive leg against them.
	USuperFAISSVectorBank* AssetBank = BakeAsAsset(*this, TEXT("Primary"));
	if (AssetBank == nullptr) { return true; }
	Inspector->SetBankForTest(AssetBank);
	Inspector->ProbeNovelty(TEXT("#0"));
	TestTrue(TEXT("(setup) asset probe #0 valid"), Inspector->GetNoveltyResult().bValid);
	TestTrue(TEXT("(setup) asset row 0 (exact duplicate exists) classifies Duplicate"),
		Inspector->GetNoveltyResult().Verdict == ESuperFAISSNoveltyVerdict::Duplicate);
	Inspector->ProbeNovelty(TEXT("#10"));
	TestTrue(TEXT("(setup) asset probe #10 valid"), Inspector->GetNoveltyResult().bValid);
	TestFalse(TEXT("(setup) asset row 10 (unique geometry) does NOT classify Duplicate"),
		Inspector->GetNoveltyResult().Verdict == ESuperFAISSNoveltyVerdict::Duplicate);

	// Archive leg: SAME geometry, opened via the real "Open scratch archive..." seam, no
	// asset selected on this slot. RED TODAY: GetSelectedBank() is asset-only, so this probe
	// currently reports "no valid bank selected" regardless of the archive's content.
	TSharedRef<SSuperFAISSBankInspector> ArchiveInspector = SNew(SSuperFAISSBankInspector);
	if (!OpenTutorialArchive(*this, ArchiveInspector.Get(), TEXT("Primary"), {}, /*bSecondSlot*/ false))
	{
		return true;
	}
	ArchiveInspector->ProbeNovelty(TEXT("#0"));
	TestTrue(TEXT("SF34-003: Novelty probe on an ARCHIVE source is valid (not \"no valid bank selected\")"),
		ArchiveInspector->GetNoveltyResult().bValid);
	TestTrue(TEXT("SF34-003: archive row 0 classifies Duplicate, matching the asset oracle"),
		ArchiveInspector->GetNoveltyResult().Verdict == ESuperFAISSNoveltyVerdict::Duplicate);

	ArchiveInspector->ProbeNovelty(TEXT("#10"));
	TestTrue(TEXT("SF34-003: archive probe #10 valid"), ArchiveInspector->GetNoveltyResult().bValid);
	TestFalse(TEXT("SF34-003: archive row 10 does NOT classify Duplicate, matching the asset oracle "
		"(dim 4/8 asset/archive parity)"),
		ArchiveInspector->GetNoveltyResult().Verdict == ESuperFAISSNoveltyVerdict::Duplicate);

	return true;
}

// ===========================================================================
// SF34-006 (Coverage Model dim 6/10/11a) -- CslsMarginThreshold calibration.
//
// CORRECTED 2026-07-22: this cell was first authored with MatchK pinned to 1, on the
// reasoning that the CSLS margin's closed form is then exact and hand-derivable with no
// enumeration risk. A build-verified run came back GREEN against the 0.0f placeholder --
// but for a degenerate reason, not a correct one: at matchK=1, r_B(i) IS the forward top-1
// similarity sim(i,j) itself, and (Cosine is symmetric) r_A(j) is the SAME sim(i,j) whenever
// back-verification succeeds -- so csls(i,j) = 2*sim - sim - sim = 0 EXACTLY for EVERY
// mutually-matched pair, independent of the similarity value. This is an algebraic identity
// of matching.h's own documented formula (matching.h:61-69), true for ANY cosine-metric bank
// at matchK=1 -- it means every matched pair in ANY matchK=1 population scores margin 0.0, so
// the tutorial correspondence set could not discriminate CslsMarginThreshold at matchK=1: the
// 0.0f placeholder passed trivially and proved nothing about calibration.
//
// MatchK is now pinned to 2 -- the smallest value that breaks the degeneracy while keeping
// every r_B/r_A term a mean of exactly two hand-countable similarities (full derivation table
// in the design doc, Claude/Curie/superfaiss-3.3.1-test-design-2026-07-22.md section 1).
//
// Population (six exact-copy singleton pairs at MatchK=2, all hand-derived from the sidecar's
// closed-form cosine rule):
//   10<->Sec0, 11<->Sec1, 12<->Sec2, 16<->Sec4, 17<->Sec5: margin 0.5 -- a "clean" pair (the
//     partner's own runner-up similarity into full Primary tops out at 0.5, same as the
//     forward runner-up, so r_A == r_B == 0.75 and margin = 2*1.0 - 0.75 - 0.75 = 0.5).
//   13<->Sec3: margin 0.25 -- Primary rows 13 and 14 share the SAME tag (3,3) by deliberate
//     fixture design (ISO-B / ISO-B-dup). Sec3's own top-2 into full Primary is therefore
//     [row13: 1.0, row14: 1.0] instead of [1.0, 0.5] -- r_A(Sec3) rises to 1.0 (from the
//     0.75 the clean pairs get), which LOWERS the margin to 2*1.0 - 0.75 - 1.0 = 0.25 even
//     though row13's own forward match is exact. This is the mutual-NN margin doing its job:
//     it is flagging that row13 has a near-duplicate elsewhere (row14), a genuinely different,
//     lower-confidence situation than the five clean singleton pairs -- and margin 0.25 is
//     where that distinction becomes visible.
//   14: forward candidate is also Sec3 (same tag as row13), but back-verification fails --
//     Sec3's own single top-1 in full Primary, tie-broken by ASCENDING INDEX (matching.h's own
//     documented determinism-tier contract: "deterministic top-k selection, ties ascending
//     index"), resolves to row13, not row14 -- so row14 stays Unmatched, the same honest
//     "no true partner" outcome as row7.
//   7: unchanged conclusion from the MatchK=1 authoring (top-1 identity does not depend on
//     matchK) -- Unmatched. Forward top-1 is Secondary row 5 (similarity 0.5, one shared
//     channel direction), but Sec5's OWN top-1 in full Primary is uniquely row 17 (tag (2,3),
//     similarity 1.0) -- not row 7 -- so back-verification fails.
//
// THE CALIBRATION CLAIM (SF34-006's written basis, D-INSP-20): any threshold strictly inside
// (0.25, 0.5] separates the two clusters this population produces -- Matched for the five
// clean pairs, Ambiguous for the near-duplicate pair -- while the shipped 0.0f placeholder
// puts BOTH clusters above threshold (0.25 >= 0.0), erasing the exact distinction the margin
// exists to draw. The design doc names 0.375 (the cluster midpoint) as the recommended pin;
// Brunel's exact choice is the build-time calibration task USuperFAISSInspectorSettings.h's
// own deferral comment names, constrained to this interval by this test.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSTutorialCorrespondenceCalibrationTest,
	"SuperFAISS.D.TutorialCorrespondenceCalibration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSTutorialCorrespondenceCalibrationTest::RunTest(const FString& Parameters)
{
	FSettingsGuard Guard;
	Guard.Settings->MatchK = 2;
	Guard.Settings->SampleLimit = 64; // exceeds both tutorial banks -- every row is checked

	USuperFAISSVectorBank* Primary = BakeAsAsset(*this, TEXT("Primary"));
	USuperFAISSVectorBank* Secondary = BakeAsAsset(*this, TEXT("Secondary"));
	if (Primary == nullptr || Secondary == nullptr) { return true; }

	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	Inspector->SetBankForTest(Primary);
	Inspector->SetSecondBankForTest(Secondary);

	auto FindPair = [Inspector](int32 SourceIndexA) -> const FSuperFAISSMatchPairResult*
	{
		return Inspector->GetMatchPairResults().FindByPredicate(
			[SourceIndexA](const FSuperFAISSMatchPairResult& P) { return P.SourceIndexA == SourceIndexA; });
	};

	// First pass at a threshold far below anything the population could produce: reads the
	// REAL, kernel-computed margins for the two oracle pairs below, so later cells test the
	// ">= " operator against the ACTUAL floating-point values the metric produces (which the
	// hand derivation above predicts are close to 0.5 and 0.25 respectively, but independently
	// run kernel passes are not guaranteed bit-identical even for a symmetric metric, so
	// asserting exact literals would be brittle to legitimate summation-order noise). The row
	// identity (which SourceIndexA matches which SourceIndexB, and which rows are Unmatched)
	// IS the independent, geometry-derived oracle and does not depend on this reasoning.
	Guard.Settings->CslsMarginThreshold = -1.0f;
	Inspector->ComputeCorrespondence();
	float CleanMargin = 0.0f; // row 11 <-> Sec1, hand-derived 0.5
	float DupMargin = 0.0f;   // row 13 <-> Sec3, hand-derived 0.25
	{
		const FSuperFAISSMatchPairResult* Pair11 = FindPair(11);
		TestTrue(TEXT("(setup) row 11 has a match-pair result"), Pair11 != nullptr);
		if (Pair11 != nullptr)
		{
			TestEqual(TEXT("SF34-006 oracle: row11<->Sec1 partner is Secondary row 1"), Pair11->SourceIndexB, 1);
			TestTrue(TEXT("SF34-006: threshold far below the margin classifies Matched"),
				Pair11->State == ESuperFAISSMatchState::Matched);
			CleanMargin = Pair11->CslsMargin;
			TestTrue(TEXT("SF34-006 oracle: row11<->Sec1 CSLS margin is close to the hand-derived 0.5 "
				"(clean singleton pair, MatchK=2 -- design doc derivation table)"),
				FMath::Abs(CleanMargin - 0.5f) < 0.05f);
		}
		const FSuperFAISSMatchPairResult* Pair13 = FindPair(13);
		TestTrue(TEXT("(setup) row 13 has a match-pair result"), Pair13 != nullptr);
		if (Pair13 != nullptr)
		{
			TestEqual(TEXT("SF34-006 oracle: row13<->Sec3 partner is Secondary row 3"), Pair13->SourceIndexB, 3);
			TestTrue(TEXT("SF34-006: threshold far below the margin classifies Matched"),
				Pair13->State == ESuperFAISSMatchState::Matched);
			DupMargin = Pair13->CslsMargin;
			TestTrue(TEXT("SF34-006 oracle: row13<->Sec3 CSLS margin is close to the hand-derived 0.25 "
				"(the ISO-B/ISO-B-dup near-duplicate collision raises r_A -- design doc derivation table)"),
				FMath::Abs(DupMargin - 0.25f) < 0.05f);
		}
		const FSuperFAISSMatchPairResult* Pair14 = FindPair(14);
		TestTrue(TEXT("(setup) row 14 has a match-pair result"), Pair14 != nullptr);
		if (Pair14 != nullptr)
		{
			TestTrue(TEXT("SF34-006 oracle: row 14 (ISO-B-dup, loses Sec3's ascending-index "
				"back-verification tie-break to row 13) is Unmatched"),
				Pair14->State == ESuperFAISSMatchState::Unmatched);
			TestEqual(TEXT("Unmatched carries no partner index"), Pair14->SourceIndexB, -1);
		}
		const FSuperFAISSMatchPairResult* Pair7 = FindPair(7);
		TestTrue(TEXT("(setup) row 7 has a match-pair result"), Pair7 != nullptr);
		if (Pair7 != nullptr)
		{
			TestTrue(TEXT("SF34-006 oracle: row 7 (Z cluster, no Secondary duplicate) is Unmatched"),
				Pair7->State == ESuperFAISSMatchState::Unmatched);
			TestEqual(TEXT("Unmatched carries no partner index"), Pair7->SourceIndexB, -1);
		}
	}

	// THE CALIBRATION ASSERTION -- RED UNTIL A REAL DEFAULT IS PINNED. Restore the shipped,
	// UNMODIFIED CslsMarginThreshold (do not override it here) and require the two-cluster
	// classification the margin exists to draw: the clean singleton pairs confidently Matched,
	// the near-duplicate pair flagged Ambiguous. This is the written basis (the fixture's own
	// margin distribution, derived above) for whatever finite default Brunel pins -- not an
	// assertion this test invents.
	Guard.Settings->CslsMarginThreshold = Guard.OrigThreshold;
	Inspector->ComputeCorrespondence();
	{
		const FSuperFAISSMatchPairResult* Pair11 = FindPair(11);
		TestTrue(TEXT("SF34-006 CALIBRATION: at the shipped default threshold, the clean pair "
			"(row11<->Sec1, margin ~0.5) classifies Matched"),
			Pair11 != nullptr && Pair11->State == ESuperFAISSMatchState::Matched);
		const FSuperFAISSMatchPairResult* Pair13 = FindPair(13);
		TestTrue(TEXT("SF34-006 CALIBRATION (red until a real default is pinned in (0.25, 0.5]): at "
			"the shipped default threshold, the near-duplicate pair (row13<->Sec3, margin ~0.25) "
			"classifies Ambiguous -- fails under the 0.0f placeholder, which puts BOTH clusters "
			"above threshold and erases the distinction the margin exists to draw"),
			Pair13 != nullptr && Pair13->State == ESuperFAISSMatchState::Ambiguous);
	}

	// Boundary cells (below/at/above), pinned against the clean pair's OWN observed margin --
	// unaffected by which finite default eventually ships inside the calibration interval.
	Guard.Settings->CslsMarginThreshold = CleanMargin;
	Inspector->ComputeCorrespondence();
	{
		const FSuperFAISSMatchPairResult* Pair11 = FindPair(11);
		TestTrue(TEXT("SF34-006 boundary: threshold == the observed margin classifies Matched (>=, not >)"),
			Pair11 != nullptr && Pair11->State == ESuperFAISSMatchState::Matched);
	}

	// ABOVE the observed margin: the same pair now classifies Ambiguous, never Unmatched (a
	// mutual partner was found; only the CALLER's classification changed).
	Guard.Settings->CslsMarginThreshold = CleanMargin + 0.1f;
	Inspector->ComputeCorrespondence();
	{
		const FSuperFAISSMatchPairResult* Pair11 = FindPair(11);
		TestTrue(TEXT("SF34-006 boundary: threshold above the observed margin classifies Ambiguous"),
			Pair11 != nullptr && Pair11->State == ESuperFAISSMatchState::Ambiguous);
	}

	// Guard vitality (dim 11a): a hostile non-finite threshold must resolve to DEFINED
	// behavior, never a crash and never silently reclassified as Matched. IEEE754: any
	// comparison against NaN is false, so `margin >= NaN` is false for every pair --
	// every otherwise-matched pair falls to Ambiguous, never Matched, never UB.
	Guard.Settings->CslsMarginThreshold = std::numeric_limits<float>::quiet_NaN();
	Inspector->ComputeCorrespondence();
	{
		const FSuperFAISSMatchPairResult* Pair11 = FindPair(11);
		TestTrue(TEXT("SF34-006 (dim 11a guard): NaN threshold classifies Ambiguous, not Matched, not a crash"),
			Pair11 != nullptr && Pair11->State == ESuperFAISSMatchState::Ambiguous);
	}

	return true;
}

// ===========================================================================
// Poirot review 3a8c857fd4, finding 2 (Minor, coverage): the plan's dim-4/8 matrix names a
// `correspondence x archive x channel-scoped x pruned` cell that no test exercised --
// TutorialCorrespondenceCalibrationTest above drives only two ASSET banks through
// ComputeCorrespondence, so the archive branch's cross-call `excludeBits` alignment (the
// tombstone words merged with ZeroEnergyBits and fed to MutualNearestMatches, both in
// native index space) had no red test pinning it. Judged correct by inspection in the
// review (SF34-005's archive BuildAnalysisSample mirrors the asset overload exactly, and
// the M1 full-view-identity guard keeps every published row -- tombstoned or not -- at its
// native index), so this cell is expected GREEN on the current, unmodified code.
//
// Oracle: Primary row 4 (Y1, tag ChanADir=1) chanA-scoped. Among Primary's chanA=1 rows
// (4, 5, 6, 12, 18) and Secondary's chanA=1 rows (Secondary row 2 only), the two banks'
// closed-form chanA-scoped cosine (1.0 iff ChanADir matches, else 0.0 -- the same
// one-hot-after-renormalization rule TutorialArchiveChannelScopeParityTest derives) makes
// row4<->Secondary2 a clean mutual match at MatchK=2, independent of Primary row 15's
// tombstone state: row 15 (ChanADir=0) never enters either the forward pass (Secondary
// side) or the chanA=1 candidate group row4 back-verifies against, so tombstoning it
// changes neither the forward top-2 into Secondary (Secondary2: 1.0, then any
// ascending-index 0.0 tie -> r_B = mean(1.0, 0.0) = 0.5) nor the back-verification's top-2
// into full Primary (rows 4, 5 by ascending-index tie among the five chanA=1 rows -> r_A =
// mean(1.0, 1.0) = 1.0). sim(row4, Secondary2) = 1.0 (pass 1's own top-1 entry), so
// csls = 2*1.0 - 0.5 - 1.0 = 0.5 on EITHER leg -- the "equivalent live rows" parity claim:
// pruning a row the pair never touches leaves the pair's answer unchanged. Margin 0.5 sits
// above the shipped default CslsMarginThreshold (0.375), so both legs classify Matched.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSTutorialArchiveCorrespondenceParityTest,
	"SuperFAISS.D.TutorialArchiveCorrespondenceParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSTutorialArchiveCorrespondenceParityTest::RunTest(const FString& Parameters)
{
	FSettingsGuard Guard;
	Guard.Settings->MatchK = 2; // Secondary has only 6 rows; MatchK must not exceed live count
	Guard.Settings->SampleLimit = 64; // exceeds both tutorial banks -- every live row is checked
	// CslsMarginThreshold is left at the shipped default (0.375) -- the hand-derived margin
	// (0.5) is not itself the claim under test; this cell asserts asset/archive PARITY.

	auto FindPairIn = [](SSuperFAISSBankInspector& Inspector, int32 SourceIndexA)
		-> const FSuperFAISSMatchPairResult*
	{
		return Inspector.GetMatchPairResults().FindByPredicate(
			[SourceIndexA](const FSuperFAISSMatchPairResult& P) { return P.SourceIndexA == SourceIndexA; });
	};

	// The asset leg: whole, unpruned Primary (row 15 present) -- the baseline "equivalent
	// live rows" answer, computed the already-proven asset way.
	USuperFAISSVectorBank* AssetPrimary = BakeAsAsset(*this, TEXT("Primary"));
	USuperFAISSVectorBank* AssetSecondary = BakeAsAsset(*this, TEXT("Secondary"));
	if (AssetPrimary == nullptr || AssetSecondary == nullptr) { return true; }

	TSharedRef<SSuperFAISSBankInspector> AssetInspector = SNew(SSuperFAISSBankInspector);
	AssetInspector->SetBankForTest(AssetPrimary);
	AssetInspector->SetAnalysisScopeForTest(TEXT("chanA"));
	AssetInspector->SetSecondBankForTest(AssetSecondary);
	AssetInspector->ComputeCorrespondence();

	const FSuperFAISSMatchPairResult* AssetPair = FindPairIn(*AssetInspector, 4);
	TestTrue(TEXT("(setup) asset leg: row 4 has a match-pair result"), AssetPair != nullptr);
	if (AssetPair == nullptr) { return true; }
	TestEqual(TEXT("(setup) asset leg oracle: row4<->Secondary2"), AssetPair->SourceIndexB, 2);
	TestTrue(TEXT("(setup) asset leg oracle: row4<->Secondary2 classifies Matched"),
		AssetPair->State == ESuperFAISSMatchState::Matched);
	TestTrue(TEXT("(setup) asset leg oracle: margin close to the hand-derived 0.5"),
		FMath::Abs(AssetPair->CslsMargin - 0.5f) < 1e-4f);

	// The archive leg: the coverage cell itself -- Primary opened as a TOMBSTONED (row 15),
	// channel-scoped ARCHIVE source, Secondary still an asset (View C's asset/archive
	// mixing is the production shape -- correspondence between two banks of different
	// source kinds).
	TSharedRef<SSuperFAISSBankInspector> ArchiveInspector = SNew(SSuperFAISSBankInspector);
	USuperFAISSVectorBank* ArchivePrimaryAsset = BakeAsAsset(*this, TEXT("Primary"));
	if (ArchivePrimaryAsset == nullptr) { return true; }
	ArchiveInspector->SetBankForTest(ArchivePrimaryAsset); // populates ProjectionScopes (channel names)
	if (!OpenTutorialArchive(*this, ArchiveInspector.Get(), TEXT("Primary"), {15}, /*bSecondSlot*/ false))
	{
		return true;
	}
	ArchiveInspector->SetAnalysisScopeForTest(TEXT("chanA"));
	const FSuperFAISSInspectionSource ArchiveSource = ArchiveInspector->GetPrimarySource();
	TestEqual(TEXT("(setup) primary source resolves to Archive kind after opening"),
		static_cast<int32>(ArchiveSource.Kind), static_cast<int32>(FSuperFAISSInspectionSource::EKind::Archive));
	TestEqual(TEXT("(setup) live count is 21 after tombstoning row 15"), ArchiveSource.GetLiveCount(), 21);

	USuperFAISSVectorBank* ArchiveSecondaryAsset = BakeAsAsset(*this, TEXT("Secondary"));
	if (ArchiveSecondaryAsset == nullptr) { return true; }
	ArchiveInspector->SetSecondBankForTest(ArchiveSecondaryAsset);
	ArchiveInspector->ComputeCorrespondence();

	const FSuperFAISSMatchPairResult* ArchivePair = FindPairIn(*ArchiveInspector, 4);
	TestTrue(TEXT("dim 4/8 coverage cell: pruned, channel-scoped ARCHIVE source row 4 has a "
		"match-pair result"), ArchivePair != nullptr);
	if (ArchivePair == nullptr) { return true; }
	TestEqual(TEXT("dim 4/8 coverage cell: archive leg row4<->Secondary2, matching the asset oracle"),
		ArchivePair->SourceIndexB, 2);
	TestTrue(TEXT("dim 4/8 coverage cell: archive leg classifies Matched, matching the asset oracle"),
		ArchivePair->State == ESuperFAISSMatchState::Matched);

	// THE PARITY ASSERTION (Poirot review finding 2): the archive leg's own margin equals
	// the asset leg's, within the same float-noise tolerance the calibration test above
	// uses -- pruning row 15 (untouched by this pair, on either the forward or the
	// back-verification pass) leaves the answer identical, proving the archive
	// BuildAnalysisSample/excludeBits path the review flagged as untested produces the
	// SAME correspondence answer as the already-proven asset path for an equivalent live
	// row.
	TestTrue(TEXT("SF34-005/Poirot-F2: asset<->archive CSLS margin parity for an equivalent live row"),
		FMath::Abs(ArchivePair->CslsMargin - AssetPair->CslsMargin) < 1e-4f);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
