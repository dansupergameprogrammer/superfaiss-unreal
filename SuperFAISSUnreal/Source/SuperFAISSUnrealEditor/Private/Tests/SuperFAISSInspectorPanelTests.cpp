// Bank Inspector I. First landed: USuperFAISSInspectorSettings + the chunked
// slow-task scaffold (progress/cancel) + View A (Structure) + View B (Novelty
// probe) — reviewed as built code. Second pass adds: MatchK/CslsMarginThreshold on
// the settings object, and View C (Correspondence) — the second-bank slot, the
// compatibility rejection matrix, the matched-pair list, and the full invalidation
// matrix (WITHOUT the archive-swap leg — the inspection-source abstraction was not
// yet started) — reviewed as built code. Third pass adds: the inspection-source
// abstraction (FSuperFAISSInspectionSource, `SSuperFAISSBankInspector.h`)
// generalizing the widget's data source from USuperFAISSVectorBank-only to EITHER a
// registry asset OR a transient, editor-private archive-loaded
// USuperFAISSScratchBank; the "Open scratch archive..." affordance
// (OpenScratchArchiveFromBytes / OpenSecondScratchArchiveFromBytes); the archive
// rejection matrix; the archive-swap leg of the reset/invalidation matrix;
// ComputeStructure()/ComputeCorrespondence() re-wired to read either source (real,
// both slots); and the space-law crux left DELIBERATELY red in
// BuildAnalysisSample(Source, ...) — sample-scoped tombstone-free-by-construction
// striding is not yet built, so a PRUNED archive's tombstoned rows leak into every
// sample-scoped pass, exactly the fixture the crux FEATs below prove against.
// Coverage Model: the ARCHIVE-related cells this pass owns (deferred out of the
// earlier passes per their own test-design artifacts). ProbeNovelty()'s own archive
// wiring is explicitly OUT OF SCOPE this round (the subsystem's
// MakeCentroidQuery/QuerySync query API is asset-typed with no drop-in archive
// equivalent for centroid construction -- a second, independent piece of new logic
// beyond the space law this pass frames as the crux) and is routed onward, not
// silently dropped -- see the test-design artifact's scope section.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SSuperFAISSBankInspector.h"
#include "SuperFAISSInspectorSettings.h"
#include "SuperFAISSInspectorSlowTask.h"
#include "SuperFAISSScratchBank.h"
#include "SuperFAISSVectorBank.h"
#include "superfaiss/superfaiss.h"

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

	// The Structure FEAT geometry (TestM1StructureFeat): three tight far-apart
	// blobs, D well-separated exact-duplicate blocks, two planted isolates. Ported
	// verbatim (not re-derived) with the row counts SCALED UP so the total exceeds the
	// widget's sample cap (the fixture-engagement law applied at the
	// setting that matters HERE: the widget's SampleLimit, not the core query path's
	// chunk boundary the core-level fixture engaged).
	void PushBlob(TArray<float>& Rows, int32 Dims, const TArray<float>& Centre,
		int32 Count, FRandomStream& Rng, float Jitter)
	{
		for (int32 i = 0; i < Count; ++i)
		{
			for (int32 d = 0; d < Dims; ++d)
			{
				Rows.Add(Centre[d] + Rng.FRandRange(-Jitter, Jitter));
			}
		}
	}

	// Builds an in-memory bank via InitFromSource (the D-group test convention — no
	// asset-registry round trip). Fails the test and returns nullptr on bake failure.
	USuperFAISSVectorBank* MakeBank(FAutomationTestBase& Test, const TArray<float>& Rows,
		int32 Count, int32 Dims, ESuperFAISSBankMetric Metric,
		ESuperFAISSBankQuantization Quant, TConstArrayView<FName> ChannelNames = {},
		TConstArrayView<int32> ChannelOffsets = {}, TConstArrayView<int32> ChannelLengths = {})
	{
		USuperFAISSVectorBank* Bank = NewObject<USuperFAISSVectorBank>();
		FString Error;
		const bool bOk = Bank->InitFromSource(Rows, Count, Dims, Metric, Quant, {},
			TEXT("inspector-panel-test"), Error, ChannelNames, ChannelOffsets, ChannelLengths);
		Test.TestTrue(FString::Printf(TEXT("bank built: %s"), *Error), bOk);
		return bOk ? Bank : nullptr;
	}

	// Slot 4b: builds a scratch archive's SAVED BYTES in-memory (no file I/O) -- Init a
	// transient USuperFAISSScratchBank at exactly Count capacity, Append every row of
	// Rows (Count x Dims, unpadded), tombstone (Remove) every index named in
	// RowsToRemove, then SaveToBytes. This is the PRUNED-archive fixture primitive every
	// crux cell below builds on: "grown with planted extra rows, then Remove'd... and
	// serialized" (the PRUNED fixture requirement). Fails the
	// test and returns false on any step failure.
	bool MakeScratchArchiveBytes(FAutomationTestBase& Test, const TArray<float>& Rows, int32 Count,
		int32 Dims, ESuperFAISSBankMetric Metric, ESuperFAISSBankQuantization Quant,
		TConstArrayView<int32> RowsToRemove, TArray<uint8>& OutBytes)
	{
		USuperFAISSScratchBank* Scratch = NewObject<USuperFAISSScratchBank>();
		if (!Scratch->Init(Count, Dims, Metric, Quant))
		{
			Test.AddError(TEXT("scratch archive fixture: Init failed"));
			return false;
		}
		for (int32 i = 0; i < Count; ++i)
		{
			TArray<float> Row;
			Row.Append(&Rows[static_cast<int64>(i) * Dims], Dims);
			int32 OutIndex = INDEX_NONE;
			if (!Scratch->Append(Row, OutIndex))
			{
				Test.AddError(FString::Printf(TEXT("scratch archive fixture: Append row %d failed"), i));
				return false;
			}
		}
		for (const int32 Idx : RowsToRemove)
		{
			if (!Scratch->Remove(Idx))
			{
				Test.AddError(FString::Printf(TEXT("scratch archive fixture: Remove row %d failed"), Idx));
				return false;
			}
		}
		if (!Scratch->SaveToBytes(OutBytes))
		{
			Test.AddError(TEXT("scratch archive fixture: SaveToBytes failed"));
			return false;
		}
		return true;
	}
}

// ===========================================================================
// USuperFAISSInspectorSettings (section 25.3; Coverage Model section 25.9 dim 2 audit
// G-1, dim 7, dim 9).
// ===========================================================================

// Dim 2 (audit G-1): the settings object is a trust boundary — every field is clamped
// to its documented range at read. Documented ranges this round derives from G-1's own
// three named examples: SampleLimit in [1, kHardSampleCap] ("cap above the hard max"),
// NoveltyLambda in [0, 1] (stated explicitly), and every k-shaped field (StructureK,
// MinComponentSize, NoveltyK — "zero/negative k's") floored at 1. No ceiling is
// documented for the k-shaped fields, so no cell asserts one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorSettingsHostileClampTest,
	"SuperFAISS.D.InspectorSettingsHostileClamp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorSettingsHostileClampTest::RunTest(const FString& Parameters)
{
	USuperFAISSInspectorSettings* Settings = NewObject<USuperFAISSInspectorSettings>();

	// SampleLimit: floor and the hard-cap ceiling.
	Settings->SampleLimit = -5;
	TestTrue(TEXT("SampleLimit floor: hostile negative clamps to >= 1"),
		Settings->GetClampedSampleLimit() >= 1);
	Settings->SampleLimit = 999999;
	TestTrue(TEXT("SampleLimit ceiling: hostile-large clamps to <= hard cap"),
		Settings->GetClampedSampleLimit() <= USuperFAISSInspectorSettings::kHardSampleCap);

	// StructureK / MinComponentSize / NoveltyK: floor only.
	Settings->StructureK = 0;
	TestTrue(TEXT("StructureK floor: zero clamps to >= 1"), Settings->GetClampedStructureK() >= 1);
	Settings->StructureK = -3;
	TestTrue(TEXT("StructureK floor: negative clamps to >= 1"), Settings->GetClampedStructureK() >= 1);
	Settings->MinComponentSize = 0;
	TestTrue(TEXT("MinComponentSize floor: zero clamps to >= 1"),
		Settings->GetClampedMinComponentSize() >= 1);
	Settings->NoveltyK = -1;
	TestTrue(TEXT("NoveltyK floor: negative clamps to >= 1"), Settings->GetClampedNoveltyK() >= 1);

	// MatchK (slot 4, View C): the same k-shaped-field convention (floor at 1, no
	// documented ceiling) — it feeds the same array/workspace-sizing trust boundary as
	// StructureK/NoveltyK (matching.h's matchK-sized top-k retrieval).
	Settings->MatchK = 0;
	TestTrue(TEXT("MatchK floor: zero clamps to >= 1"), Settings->GetClampedMatchK() >= 1);
	Settings->MatchK = -7;
	TestTrue(TEXT("MatchK floor: negative clamps to >= 1"), Settings->GetClampedMatchK() >= 1);

	// NoveltyLambda: documented [0, 1] range, both edges.
	Settings->NoveltyLambda = -0.5f;
	TestTrue(TEXT("NoveltyLambda floor: negative clamps to >= 0"),
		Settings->GetClampedNoveltyLambda() >= 0.0f);
	Settings->NoveltyLambda = 1.5f;
	TestTrue(TEXT("NoveltyLambda ceiling: > 1 clamps to <= 1"),
		Settings->GetClampedNoveltyLambda() <= 1.0f);

	// Positive control: valid values pass through unchanged (guards against a clamp
	// that over-corrects a legal value — must hold both before AND after the
	// real clamp is wired in).
	Settings->SampleLimit = 2048;
	Settings->StructureK = 16;
	Settings->MinComponentSize = 3;
	Settings->NoveltyK = 8;
	Settings->NoveltyLambda = 0.95f;
	Settings->MatchK = 10;
	TestEqual(TEXT("valid SampleLimit passes through"), Settings->GetClampedSampleLimit(), 2048);
	TestEqual(TEXT("valid StructureK passes through"), Settings->GetClampedStructureK(), 16);
	TestEqual(TEXT("valid MinComponentSize passes through"), Settings->GetClampedMinComponentSize(), 3);
	TestEqual(TEXT("valid NoveltyK passes through"), Settings->GetClampedNoveltyK(), 8);
	TestEqual(TEXT("valid NoveltyLambda passes through"), Settings->GetClampedNoveltyLambda(), 0.95f);
	TestEqual(TEXT("valid MatchK passes through"), Settings->GetClampedMatchK(), 10);
	return true;
}

// Dim 7 (boundary — performance): the default-budget contract, EffectiveDefaultSample =
// min(ClampedSampleLimit, floor(sqrt(BudgetMacs / Dims))), asserted at constructed dims
// extremes {304, 768, 1536} against the FORMULA — BudgetMacs here is an arbitrary
// stand-in constant, not V32-V1's real calibrated value (that calibration is a slot-6
// measurement task this cell does not depend on; it is handed the constant, never
// derives it).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorSettingsEffectiveDefaultSampleTest,
	"SuperFAISS.D.InspectorSettingsEffectiveDefaultSample",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorSettingsEffectiveDefaultSampleTest::RunTest(const FString& Parameters)
{
	USuperFAISSInspectorSettings* Settings = NewObject<USuperFAISSInspectorSettings>();
	Settings->SampleLimit = 8192;
	const int64 BudgetMacs = 100000000; // an arbitrary stand-in constant (see comment above)

	for (int32 Dims : {304, 768, 1536})
	{
		const int32 Expected = FMath::Min(Settings->GetClampedSampleLimit(),
			FMath::FloorToInt(FMath::Sqrt(static_cast<double>(BudgetMacs) / Dims)));
		TestEqual(FString::Printf(TEXT("EffectiveDefaultSample dims=%d"), Dims),
			Settings->EffectiveDefaultSample(Dims, BudgetMacs), Expected);
	}

	// The min() branch: a tiny dims + huge budget must clamp to SampleLimit, never
	// exceed it.
	const int32 Clamped = Settings->EffectiveDefaultSample(8, 1000000000000ll);
	TestTrue(TEXT("EffectiveDefaultSample never exceeds ClampedSampleLimit"),
		Clamped <= Settings->GetClampedSampleLimit());
	return true;
}

// Dim 9 (persistence round-trip, narrow): the settings object round-trips its config
// file (save -> restart -> load, values intact). This cell is GREEN AT AUTHORING TIME —
// UPROPERTY(config) round-tripping is provided by UObject/UDeveloperSettings reflection
// once the fields are declared correctly; there is no achievement left to
// gate here, only the declaration this round already authored. Kept as a standing
// regression guard, not a red-unimplemented cell. Snapshots + restores the CDO's real
// values so a live project's saved ini is not left polluted by the test run.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorSettingsPersistenceTest,
	"SuperFAISS.D.InspectorSettingsPersistence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorSettingsPersistenceTest::RunTest(const FString& Parameters)
{
	USuperFAISSInspectorSettings* Settings = GetMutableDefault<USuperFAISSInspectorSettings>();
	const int32 OrigSampleLimit = Settings->SampleLimit;
	const int32 OrigStructureK = Settings->StructureK;
	const int32 OrigMinComponentSize = Settings->MinComponentSize;
	const int32 OrigNoveltyK = Settings->NoveltyK;
	const float OrigNoveltyLambda = Settings->NoveltyLambda;

	Settings->SampleLimit = 4096;
	Settings->StructureK = 12;
	Settings->MinComponentSize = 5;
	Settings->NoveltyK = 6;
	Settings->NoveltyLambda = 0.80f;
	Settings->SaveConfig();

	// Corrupt the in-memory CDO, then reload from the just-saved ini: values must
	// round-trip through the file, not merely survive in memory.
	Settings->SampleLimit = -1;
	Settings->StructureK = -1;
	Settings->MinComponentSize = -1;
	Settings->NoveltyK = -1;
	Settings->NoveltyLambda = -1.0f;
	Settings->LoadConfig();

	TestEqual(TEXT("SampleLimit round-trips"), Settings->SampleLimit, 4096);
	TestEqual(TEXT("StructureK round-trips"), Settings->StructureK, 12);
	TestEqual(TEXT("MinComponentSize round-trips"), Settings->MinComponentSize, 5);
	TestEqual(TEXT("NoveltyK round-trips"), Settings->NoveltyK, 6);
	TestEqual(TEXT("NoveltyLambda round-trips"), Settings->NoveltyLambda, 0.80f);

	Settings->SampleLimit = OrigSampleLimit;
	Settings->StructureK = OrigStructureK;
	Settings->MinComponentSize = OrigMinComponentSize;
	Settings->NoveltyK = OrigNoveltyK;
	Settings->NoveltyLambda = OrigNoveltyLambda;
	Settings->SaveConfig();
	return true;
}

// ===========================================================================
// SuperFAISSInspectorSlowTask::RunChunked (section 25.3; Coverage Model section 25.9
// dim 3 (applicable narrowly for M4) + dim 5 (the cancel path is a first-class
// rejection cell) + the dim-10 boundary's "chunked, progress ticks, cancel
// reachable and effective mid-pass" editor cell.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorSlowTaskChunkingTest,
	"SuperFAISS.D.InspectorSlowTaskChunking",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorSlowTaskChunkingTest::RunTest(const FString& Parameters)
{
	using namespace SuperFAISSInspectorSlowTask;

	// Evenly-divisible range: exactly RowCount/ChunkSize chunks, each ChunkSize rows.
	{
		TArray<TPair<int32, int32>> Chunks;
		const FResult Result = RunChunked(FText::FromString(TEXT("test")), 1000, 100,
			[&Chunks](int32 ChunkStart, int32 ChunkCount) { Chunks.Emplace(ChunkStart, ChunkCount); },
			[]() { return false; });
		TestEqual(TEXT("evenly-divisible: chunk count"), Result.ChunksProcessed, 10);
		TestEqual(TEXT("evenly-divisible: rows processed"), Result.RowsProcessed, 1000);
		TestTrue(TEXT("evenly-divisible: completed"), Result.bCompleted);
		TestFalse(TEXT("evenly-divisible: not cancelled"), Result.bCancelled);
		TestEqual(TEXT("evenly-divisible: chunk ranges"), Chunks.Num(), 10);
		if (Chunks.Num() == 10)
		{
			TestEqual(TEXT("first chunk start"), Chunks[0].Key, 0);
			TestEqual(TEXT("first chunk count"), Chunks[0].Value, 100);
			TestEqual(TEXT("last chunk start"), Chunks[9].Key, 900);
			TestEqual(TEXT("last chunk count"), Chunks[9].Value, 100);
		}
	}
	// Non-divisible range: the final chunk is a partial remainder.
	{
		TArray<TPair<int32, int32>> Chunks;
		const FResult Result = RunChunked(FText::FromString(TEXT("test")), 950, 100,
			[&Chunks](int32 ChunkStart, int32 ChunkCount) { Chunks.Emplace(ChunkStart, ChunkCount); },
			[]() { return false; });
		TestEqual(TEXT("non-divisible: chunk count"), Result.ChunksProcessed, 10);
		TestEqual(TEXT("non-divisible: rows processed"), Result.RowsProcessed, 950);
		if (Chunks.Num() == 10)
		{
			TestEqual(TEXT("non-divisible: final chunk start"), Chunks[9].Key, 900);
			TestEqual(TEXT("non-divisible: final chunk count"), Chunks[9].Value, 50);
		}
	}
	// Degenerate: RowCount == 0 -> no chunks, trivially completed.
	{
		int32 Calls = 0;
		const FResult Result = RunChunked(FText::FromString(TEXT("test")), 0, 100,
			[&Calls](int32, int32) { ++Calls; }, []() { return false; });
		TestEqual(TEXT("zero rows: no ChunkFn calls"), Calls, 0);
		TestEqual(TEXT("zero rows: zero chunks processed"), Result.ChunksProcessed, 0);
		TestTrue(TEXT("zero rows: trivially completed"), Result.bCompleted);
	}
	// Degenerate: ChunkSize < 1 is treated as RowCount (one chunk).
	{
		const FResult Result = RunChunked(FText::FromString(TEXT("test")), 500, 0,
			[](int32, int32) {}, []() { return false; });
		TestEqual(TEXT("ChunkSize<1: one chunk"), Result.ChunksProcessed, 1);
		TestEqual(TEXT("ChunkSize<1: all rows processed"), Result.RowsProcessed, 500);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorSlowTaskCancelTest,
	"SuperFAISS.D.InspectorSlowTaskCancel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorSlowTaskCancelTest::RunTest(const FString& Parameters)
{
	using namespace SuperFAISSInspectorSlowTask;

	// Cancel after 3 chunk boundaries: exactly 3 chunks run, 300 rows processed, no
	// chunk beyond the cancel point ever reaches ChunkFn (the "no partial cache
	// commit" contract's own proof: a caller accumulating into an array sees exactly
	// the processed rows, never more).
	{
		int32 ChunksSeen = 0;
		TArray<int32> Written;
		const FResult Result = RunChunked(FText::FromString(TEXT("test")), 1000, 100,
			[&Written](int32 ChunkStart, int32 ChunkCount)
			{
				for (int32 i = 0; i < ChunkCount; ++i) { Written.Add(ChunkStart + i); }
			},
			[&ChunksSeen]() { return ChunksSeen++ >= 3; });
		TestEqual(TEXT("cancel-after-3: chunks processed"), Result.ChunksProcessed, 3);
		TestEqual(TEXT("cancel-after-3: rows processed"), Result.RowsProcessed, 300);
		TestTrue(TEXT("cancel-after-3: bCancelled"), Result.bCancelled);
		TestFalse(TEXT("cancel-after-3: NOT bCompleted (mutually exclusive)"), Result.bCompleted);
		TestEqual(TEXT("cancel-after-3: no partial write beyond the cancel point"),
			Written.Num(), 300);
	}
	// Cancel on the very first boundary check (before any chunk runs): zero rows,
	// zero chunks, still a defined, non-crashing cancel.
	{
		int32 Calls = 0;
		const FResult Result = RunChunked(FText::FromString(TEXT("test")), 1000, 100,
			[&Calls](int32, int32) { ++Calls; }, []() { return true; });
		TestEqual(TEXT("cancel-immediately: no ChunkFn calls"), Calls, 0);
		TestEqual(TEXT("cancel-immediately: zero rows processed"), Result.RowsProcessed, 0);
		TestEqual(TEXT("cancel-immediately: zero chunks processed"), Result.ChunksProcessed, 0);
		TestTrue(TEXT("cancel-immediately: bCancelled"), Result.bCancelled);
	}
	// A subsequent, never-cancelled full run over the SAME row count produces output
	// bit-identical to a run that was never preceded by a cancelled attempt (the
	// contract: "a subsequent full run produces output bit-identical to a never-cancelled run").
	{
		TArray<int32> AfterCancelRun;
		int32 CancelOnce = 0;
		RunChunked(FText::FromString(TEXT("test")), 500, 50,
			[](int32, int32) {}, [&CancelOnce]() { return CancelOnce++ >= 1; });
		const FResult Full = RunChunked(FText::FromString(TEXT("test")), 500, 50,
			[&AfterCancelRun](int32 ChunkStart, int32 ChunkCount)
			{
				for (int32 i = 0; i < ChunkCount; ++i) { AfterCancelRun.Add(ChunkStart + i); }
			},
			[]() { return false; });
		TArray<int32> NeverCancelledRun;
		RunChunked(FText::FromString(TEXT("test")), 500, 50,
			[&NeverCancelledRun](int32 ChunkStart, int32 ChunkCount)
			{
				for (int32 i = 0; i < ChunkCount; ++i) { NeverCancelledRun.Add(ChunkStart + i); }
			},
			[]() { return false; });
		TestTrue(TEXT("post-cancel full run completed"), Full.bCompleted);
		TestTrue(TEXT("post-cancel full run bit-identical to a never-cancelled run"),
			AfterCancelRun == NeverCancelledRun);
	}
	return true;
}

// ===========================================================================
// SSuperFAISSBankInspector — View A (Structure) / View B (Novelty probe), section
// 25.5. Coverage Model section 25.9's M4 dimension, scoped to slot 3.
// ===========================================================================

// Dim 5: status lines carry the documented diagnostic text — "no valid bank" — for
// both panels.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorNoBankRejectionTest,
	"SuperFAISS.D.InspectorNoBankRejection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorNoBankRejectionTest::RunTest(const FString& Parameters)
{
	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	Inspector->SetBankForTest(nullptr);

	Inspector->ComputeStructure();
	TestEqual(TEXT("Structure: no bank selected status"), Inspector->GetStructureStatus(),
		FString(TEXT("no valid bank selected")));

	Inspector->ProbeNovelty(TEXT("#0"));
	TestEqual(TEXT("Novelty: no bank selected status"), Inspector->GetNoveltyStatus(),
		FString(TEXT("no valid bank selected")));

	// Correspondence (slot 4): the same "no valid bank selected" idiom for the PRIMARY
	// bank precondition, checked before the second-bank slot is even consulted.
	Inspector->ComputeCorrespondence();
	TestEqual(TEXT("Correspondence: no primary bank selected status"), Inspector->GetCorrespondenceStatus(),
		FString(TEXT("no valid bank selected")));
	return true;
}

// Dim 7 (doc cell): the View A determinism-tier disclosure copy is present verbatim.
// GREEN AT AUTHORING TIME — a literal contract string, not an achievement (see the
// file header note); kept as a standing regression guard against transcription drift.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorStructureDisclosureCopyTest,
	"SuperFAISS.D.InspectorStructureDisclosureCopy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorStructureDisclosureCopyTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("View A disclosure copy, verbatim (section 25.5)"),
		FString(SSuperFAISSBankInspector::StructureDisclosureCopy()),
		FString(TEXT("Deterministic on this device (fixed sample, fixed order). Layouts ")
			TEXT("and cluster ids may differ across machines — no cross-device claim.")));
	return true;
}

// Dim 1 (audit N-2): the coarse, one-rule cache invalidation — primary re-select and
// an analysis-scope change both clear BOTH the Structure and Novelty caches together.
// "archive-swap without widget close" (audit F3) is explicitly OUT OF SCOPE for slot 3
// (no inspection-source abstraction exists yet — slot 4b) and is not celled here; the
// second-bank axis (View C) is likewise slot 4 and not celled here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorCacheInvalidationMatrixTest,
	"SuperFAISS.D.InspectorCacheInvalidationMatrix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorCacheInvalidationMatrixTest::RunTest(const FString& Parameters)
{
	const TArray<float> Rows = SeededRows(20, 8, 0x1A1A);
	USuperFAISSVectorBank* BankA = MakeBank(*this, Rows, 20, 8,
		ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Float32);
	const TArray<FName> ChannelNames = {TEXT("chanA"), TEXT("chanB")};
	const TArray<int32> ChannelOffsets = {0, 16};
	const TArray<int32> ChannelLengths = {16, 16};
	USuperFAISSVectorBank* BankB = MakeBank(*this, SeededRows(20, 32, 0x1B1B), 20, 32,
		ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Int8,
		ChannelNames, ChannelOffsets, ChannelLengths);
	if (BankA == nullptr || BankB == nullptr)
	{
		return true;
	}

	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);

	// Establish a real "previously computed" state (the scaffold's distinct poison,
	// section 25.9 dim 1's own precondition — an already-empty cache proves nothing
	// about whether invalidation fired) before re-selecting the SAME bank.
	Inspector->SetBankForTest(BankA);
	Inspector->ComputeStructure();
	Inspector->ProbeNovelty(TEXT("#0"));
	TestTrue(TEXT("(setup) a poison Structure result exists before re-select"),
		Inspector->GetStructureClusters().Num() > 0);
	TestTrue(TEXT("(setup) a Novelty result exists before re-select"),
		Inspector->GetNoveltyResult().bValid);

	Inspector->SetBankForTest(BankA);
	TestEqual(TEXT("re-select: Structure cache clear after bank selection"),
		Inspector->GetStructureClusters().Num(), 0);
	TestFalse(TEXT("re-select: Novelty verdict not stale-valid after bank selection"),
		Inspector->GetNoveltyResult().bValid);

	// Selecting a channel-carrying bank populates the scope combo; switching scope
	// must also clear the caches (dim 1's "not only the metric axis" clause).
	Inspector->SetBankForTest(BankB);
	Inspector->ComputeStructure();
	Inspector->ProbeNovelty(TEXT("#0"));
	TestTrue(TEXT("(setup) a poison Structure result exists before the scope change"),
		Inspector->GetStructureClusters().Num() > 0);
	TestTrue(TEXT("(setup) a Novelty result exists before the scope change"),
		Inspector->GetNoveltyResult().bValid);

	Inspector->SetAnalysisScopeForTest(TEXT("chanA"));
	TestEqual(TEXT("scope change: Structure cache clear after scope change"),
		Inspector->GetStructureClusters().Num(), 0);
	TestFalse(TEXT("scope change: Novelty verdict not stale-valid after scope change"),
		Inspector->GetNoveltyResult().bValid);
	return true;
}

// Dim 10 (crux, audit G-6): the Structure FEAT's constructed geometry, imported as an
// in-memory bank and driven THROUGH THE WIDGET — "Compute structure" reports exactly
// the constructed component count and memberships, and the ids join 1:1 onto the
// rendered scatter sample. ONE representative block count (D=3) is run here, not the
// full D in {2,3,5} sweep — block-count generality is already proven at the core level
// (M1 slot 0, TestM1StructureFeat); this cell's incremental claim is the widget-level
// wiring and the scatter join (audit G-6's own "non-empty alone is not proof" concern),
// not block-count generality. Row counts are scaled up from the core fixture's 400/blob
// to 800/blob so the total (2414 for D=3) exceeds the widget's default SampleLimit
// (2048) — the fixture-engagement law applied at the SETTING THAT MATTERS here (the
// widget's own sample cap, distinct from the core fixture's chunk-boundary target).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorStructurePanelFeatTest,
	"SuperFAISS.D.InspectorStructurePanelFeat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorStructurePanelFeatTest::RunTest(const FString& Parameters)
{
	FRandomStream Rng(0xFEA7);
	const int32 Dims = 64;
	const int32 BlobSize = 800; // 3 blobs = 2400 rows -- above the widget's SampleLimit=2048
	const int32 D = 3; // representative duplicate-block count (see comment above)
	const int32 MinComp = 3;

	TArray<float> Rows;
	TArray<int32> Planted; // ground truth: component tag per row, -1 = isolate
	int32 CompTag = 0;
	for (int32 Bl = 0; Bl < 3; ++Bl)
	{
		TArray<float> Centre;
		Centre.SetNumZeroed(Dims);
		Centre[Bl * 5] = 50.0f * (Bl + 1);
		PushBlob(Rows, Dims, Centre, BlobSize, Rng, 0.05f);
		for (int32 i = 0; i < BlobSize; ++i) { Planted.Add(CompTag); }
		++CompTag;
	}
	for (int32 d = 0; d < D; ++d)
	{
		TArray<float> C;
		C.SetNumZeroed(Dims);
		C[Dims - 1 - d] = 500.0f * (d + 1);
		for (int32 i = 0; i < 6; ++i) { Rows.Append(C); Planted.Add(CompTag); }
		++CompTag;
	}
	for (int32 Iso = 0; Iso < 2; ++Iso)
	{
		TArray<float> C;
		C.SetNumZeroed(Dims);
		C[Dims / 2 + Iso] = 9000.0f * (Iso + 1);
		Rows.Append(C);
		Planted.Add(-1);
	}
	const int32 Count = Planted.Num();
	TestTrue(TEXT("fixture exceeds the widget's default SampleLimit (2048)"), Count > 2048);

	USuperFAISSVectorBank* Bank = MakeBank(*this, Rows, Count, Dims,
		ESuperFAISSBankMetric::L2, ESuperFAISSBankQuantization::Int8);
	if (Bank == nullptr)
	{
		return true;
	}

	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	Inspector->SetBankForTest(Bank);
	Inspector->ComputeStructure();

	int32 BigComponents = 0;
	for (const FSuperFAISSStructureCluster& Cluster : Inspector->GetStructureClusters())
	{
		if (Cluster.MemberSampleIndices.Num() >= MinComp) { ++BigComponents; }
	}
	TestEqual(TEXT("Panel FEAT: exactly (3 + D) components >= MinComponentSize"),
		BigComponents, 3 + D);
	TestEqual(TEXT("Panel FEAT: exactly 2 isolates folded into the outliers row"),
		Inspector->GetStructureOutlierSampleIndices().Num(), 2);

	// The 1:1 scatter join (audit G-6): the component-id array is sized to the SAMPLED
	// row count the widget's shared cap admits (the default settings SampleLimit,
	// 2048 -- not the full bank count, Count), one entry per rendered scatter point.
	TestEqual(TEXT("Panel FEAT: component-id array sized to the sample, not the full bank"),
		Inspector->GetStructureComponentIdBySampleIndex().Num(), FMath::Min(Count, 2048));
	return true;
}

// Dim 8 (composition): Structure x channel scope — components computed on a channel
// slice, driven through the same scope selector the projection uses.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorStructureChannelScopeCompositionTest,
	"SuperFAISS.D.InspectorStructureChannelScopeComposition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorStructureChannelScopeCompositionTest::RunTest(const FString& Parameters)
{
	const TArray<FName> ChannelNames = {TEXT("chanA"), TEXT("chanB")};
	const TArray<int32> ChannelOffsets = {0, 16};
	const TArray<int32> ChannelLengths = {16, 16};
	USuperFAISSVectorBank* Bank = MakeBank(*this, SeededRows(64, 32, 0xC4A0), 64, 32,
		ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Int8,
		ChannelNames, ChannelOffsets, ChannelLengths);
	if (Bank == nullptr)
	{
		return true;
	}

	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	Inspector->SetBankForTest(Bank);
	Inspector->SetAnalysisScopeForTest(TEXT("chanA"));
	Inspector->ComputeStructure();

	// The claim under test: a channel-scoped Structure pass produces components with
	// REAL MEMBERSHIP joined onto the channel-scoped sample — asserted on total
	// membership, not cluster-list non-emptiness, because a poison cluster (this cell's
	// original authoring-time scaffold concern, no longer live now that ComputeStructure
	// is real) would itself be non-empty as a LIST but carry zero members, so a bare
	// "list non-empty" check would pass on a poison alone. The channel-scope selection
	// machinery itself is pre-existing, shipped code (V32-G1-era) and is not this cell's
	// claim.
	int32 TotalMembers = 0;
	for (const FSuperFAISSStructureCluster& Cluster : Inspector->GetStructureClusters())
	{
		TotalMembers += Cluster.MemberSampleIndices.Num();
	}
	TestTrue(TEXT("Structure x channel scope: components have real membership"),
		TotalMembers > 0);
	return true;
}

// The killer this cell replaces the test above's false-green
// coverage for -- a channel-scoped Cosine sample's sliced rows must be renormalized to
// unit norm OVER THE SLICE before the plain (channel-metadata-stripped) Cosine kernel
// scores them; "TotalMembers > 0" is satisfied identically by a wrong-but-nonempty
// clustering. This is a FEAT oracle: it executes BuildAnalysisSample directly and scores
// the resulting view with the core Query() entry point, asserting the actual cosine
// similarity the kernel reports against the ground truth the slice's own direction
// defines -- never a recount of BuildAnalysisSample's own steps.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorChannelCosineSliceRenormalizationTest,
	"SuperFAISS.D.InspectorChannelCosineSliceRenormalization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorChannelCosineSliceRenormalizationTest::RunTest(const FString& Parameters)
{
	using namespace superfaiss;

	const TArray<FName> ChannelNames = {TEXT("chan0"), TEXT("chan1")};
	const TArray<int32> ChannelOffsets = {0, 16};
	const TArray<int32> ChannelLengths = {16, 16};
	const int32 Dims = 32;

	TArray<float> Rows;
	Rows.SetNumZeroed(3 * Dims);
	// Row 0: chan0 direction e0, magnitude 10; chan1 ALSO magnitude 10 (equal energy in
	// both channels) -- whole-row normalization leaves the chan0 sub-vector at 1/sqrt(2)
	// of its own unit direction, not already unit. That gap is exactly what the bug misses.
	Rows[0 * Dims + 0] = 10.0f;
	Rows[0 * Dims + 16] = 10.0f;
	// Row 1: chan0 direction e1, orthogonal to row 0's chan0 direction -- its dot product
	// against the query below is 0 regardless of renormalization, included only so the
	// sample isn't degenerate.
	Rows[1 * Dims + 1] = 10.0f;
	Rows[1 * Dims + 16] = 10.0f;
	// Row 2: ZERO energy in chan0 (an all-zero slice), nonzero elsewhere so the whole row
	// bakes fine -- the zero-energy-slice exclusion cell.
	Rows[2 * Dims + 16] = 5.0f;

	USuperFAISSVectorBank* Bank = MakeBank(*this, Rows, 3, Dims,
		ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Int8,
		ChannelNames, ChannelOffsets, ChannelLengths);
	if (Bank == nullptr)
	{
		return true;
	}

	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	Inspector->SetBankForTest(Bank);
	Inspector->SetAnalysisScopeForTest(TEXT("chan0"));

	FSuperFAISSInspectionSource Source;
	Source.Kind = FSuperFAISSInspectionSource::EKind::Asset;
	Source.Asset = Bank;

	TArray<uint8, TAlignedHeapAllocator<16>> Payload;
	TArray<float> Scales;
	BankView View;
	TArray<int32> SourceIndices;
	int32 ZeroEnergyExcluded = 0;
	const bool bBuilt = Inspector->BuildAnalysisSampleForTest(Source, 3, Payload, Scales, View,
		SourceIndices, /*bSkipTombstonedRows*/ true, &ZeroEnergyExcluded);
	TestTrue(TEXT("channel-scoped sample builds"), bBuilt);
	if (!bBuilt)
	{
		return true;
	}

	// The zero-energy exclusion: row 2 has no direction in chan0 and must be dropped from
	// the sample, with the count disclosed to the caller.
	TestEqual(TEXT("zero-energy chan0 row excluded from the sample"), View.count, 2);
	TestEqual(TEXT("zero-energy exclusion count disclosed"), ZeroEnergyExcluded, 1);
	TestFalse(TEXT("excluded row's source index is not in the sample"), SourceIndices.Contains(2));

	// The renormalization itself: query the sliced view with the EXACT chan0 unit
	// direction of row 0, (1,0,0,...,0). A correctly renormalized slice reports cosine
	// similarity ~1.0 for row 0 (same direction). The non-renormalized bug reports the
	// row's WHOLE-ROW-normalized chan0 component instead (~0.7071 here, by construction --
	// chan0 and chan1 carry equal energy, so whole-row normalization halves it between
	// them) -- a difference far outside quantization noise.
	alignas(16) float QueryBuf[16] = {};
	QueryBuf[0] = 1.0f;

	Workspace Ws;
	Hit Hits[2];
	int32_t HitCount = 0;
	QueryParams Params;
	Params.k = 2;
	const Status QueryStatus = superfaiss::Query(View, QueryBuf, Params, Ws, Hits, &HitCount);
	TestEqual(TEXT("query over the renormalized slice succeeds"),
		static_cast<int>(QueryStatus), static_cast<int>(Status::Ok));
	if (QueryStatus != Status::Ok || HitCount < 1)
	{
		return true;
	}

	TestTrue(TEXT("row 0's chan0 direction is the nearest match to its own direction"),
		SourceIndices.IsValidIndex(Hits[0].index) && SourceIndices[Hits[0].index] == 0);
	TestTrue(TEXT("renormalized chan0 slice scores ~1.0 cosine similarity against its own direction"),
		FMath::Abs(Hits[0].score - 1.0f) < 0.05f);

	return true;
}

// Finding 6 (regression on Finding 1, caught at pre-gate review): the STRUCTURAL half.
// Finding 1's compaction dropped zero-energy rows unconditionally, including on a
// FULL-VIEW IDENTITY build (bSkipTombstonedRows=false) -- the same shape the space law's
// tombstone placement already requires index identity from ("a tombstoned row must stay
// IN the view, at its native index, so the caller's excludeBits land on the right row").
// This cell pins that invariant directly at the BuildAnalysisSample level: a full-view
// identity build over a source with zero-energy rows must return EVERY row (nothing
// compacted, count == the source's own published count), with the zero-energy rows
// reported via an exclusion mask the caller ORs into its own excludeBits -- never
// silently reindexed the way a mid-sample drop would shift every row after it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorFullViewZeroEnergyIndexIdentityTest,
	"SuperFAISS.D.InspectorFullViewZeroEnergyIndexIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorFullViewZeroEnergyIndexIdentityTest::RunTest(const FString& Parameters)
{
	using namespace superfaiss;

	const TArray<FName> ChannelNames = {TEXT("chan0"), TEXT("chan1")};
	const TArray<int32> ChannelOffsets = {0, 16};
	const TArray<int32> ChannelLengths = {16, 16};
	const int32 Dims = 32;

	// Four rows; rows 1 and 3 (NOT the last row -- a mid-sample drop is the case that
	// would silently shift every later row's index under compaction) have zero energy in
	// chan0.
	TArray<float> Rows;
	Rows.SetNumZeroed(4 * Dims);
	Rows[0 * Dims + 0] = 10.0f;  Rows[0 * Dims + 16] = 5.0f; // row 0: normal
	Rows[1 * Dims + 16] = 5.0f;                              // row 1: zero energy in chan0
	Rows[2 * Dims + 1] = 10.0f;  Rows[2 * Dims + 16] = 5.0f; // row 2: normal
	Rows[3 * Dims + 16] = 5.0f;                              // row 3: zero energy in chan0

	USuperFAISSVectorBank* Bank = MakeBank(*this, Rows, 4, Dims,
		ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Int8,
		ChannelNames, ChannelOffsets, ChannelLengths);
	if (Bank == nullptr)
	{
		return true;
	}

	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	Inspector->SetBankForTest(Bank);
	Inspector->SetAnalysisScopeForTest(TEXT("chan0"));

	FSuperFAISSInspectionSource Source;
	Source.Kind = FSuperFAISSInspectionSource::EKind::Asset;
	Source.Asset = Bank;

	TArray<uint8, TAlignedHeapAllocator<16>> Payload;
	TArray<float> Scales;
	BankView View;
	TArray<int32> SourceIndices;
	int32 ZeroEnergyExcluded = 0;
	TArray<uint32> ZeroEnergyBits;
	// The full-view identity shape: SampleLimit == the source's own count,
	// bSkipTombstonedRows=false -- exactly how ComputeCorrespondence builds its full B/A
	// views.
	const bool bBuilt = Inspector->BuildAnalysisSampleForTest(Source, Bank->Count, Payload, Scales, View,
		SourceIndices, /*bSkipTombstonedRows*/ false, &ZeroEnergyExcluded, &ZeroEnergyBits);
	TestTrue(TEXT("full-view identity build succeeds"), bBuilt);
	if (!bBuilt)
	{
		return true;
	}

	TestEqual(TEXT("full-view identity: NOTHING compacted, count == source's own count"),
		View.count, Bank->Count);
	TestEqual(TEXT("full-view identity: zero-energy exclusion count disclosed"), ZeroEnergyExcluded, 2);

	// Index identity: OutSourceIndices[s] == s for every s -- no row shifted.
	TestEqual(TEXT("source index count matches"), SourceIndices.Num(), Bank->Count);
	for (int32 s = 0; s < SourceIndices.Num(); ++s)
	{
		TestEqual(FString::Printf(TEXT("native index identity at position %d"), s), SourceIndices[s], s);
	}

	// The exclusion mask: bits 1 and 3 set (the zero-energy rows), 0 and 2 clear -- in the
	// SAME native index space SourceIndices already confirmed is untouched, so a caller's
	// OR against tombstone words lands on the intended rows.
	auto BitSet = [&ZeroEnergyBits](int32 Index) -> bool
	{
		return ZeroEnergyBits.IsValidIndex(Index / 32) &&
			(ZeroEnergyBits[Index / 32] & (1u << (Index % 32))) != 0;
	};
	TestFalse(TEXT("row 0 (normal) is not excluded"), BitSet(0));
	TestTrue(TEXT("row 1 (zero energy) is excluded"), BitSet(1));
	TestFalse(TEXT("row 2 (normal) is not excluded"), BitSet(2));
	TestTrue(TEXT("row 3 (zero energy) is excluded"), BitSet(3));

	return true;
}

// Launch-gate finding M1: the full-view identity build's index-native shape (proven above)
// depends on a precondition -- bCompactZeroEnergy=false (bSkipTombstonedRows=false, one
// level up) is only ever safe when SampleLimit >= the source's own published count, so
// SampleCount == Full.count and every row is its own candidate at its own position. That
// precondition was documented in prose (Finding 6's comment) and enforced nowhere:
// BuildAnalysisSampleForTest accepted bSkipTombstonedRows=false with ANY limit, and a
// smaller one would silently compact -- masking the WRONG rows against a caller's
// native-index-space excludeBits with no signal anything went wrong. This cell proves the
// mismatch now fails LOUDLY (returns false) instead.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorFullViewIdentityLimitMismatchRejectsTest,
	"SuperFAISS.D.InspectorFullViewIdentityLimitMismatchRejects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorFullViewIdentityLimitMismatchRejectsTest::RunTest(const FString& Parameters)
{
	using namespace superfaiss;

	USuperFAISSVectorBank* Bank = MakeBank(*this, SeededRows(20, 8, 0x1101), 20, 8,
		ESuperFAISSBankMetric::L2, ESuperFAISSBankQuantization::Float32);
	if (Bank == nullptr)
	{
		return true;
	}

	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	Inspector->SetBankForTest(Bank);

	FSuperFAISSInspectionSource Source;
	Source.Kind = FSuperFAISSInspectionSource::EKind::Asset;
	Source.Asset = Bank;

	TArray<uint8, TAlignedHeapAllocator<16>> Payload;
	TArray<float> Scales;
	BankView View;
	TArray<int32> SourceIndices;

	// A non-compacting build (bSkipTombstonedRows=false) with a limit BELOW the bank's own
	// count (20 rows, limit 10) -- exactly the mismatch that used to silently compact.
	const bool bMismatched = Inspector->BuildAnalysisSampleForTest(Source, 10, Payload, Scales, View,
		SourceIndices, /*bSkipTombstonedRows*/ false);
	TestFalse(TEXT("a non-compacting build with a smaller limit fails loudly, not silently"), bMismatched);

	// Negative control: the SAME non-compacting flag with a MATCHING limit (the real,
	// already-proven shape every production call site uses) still succeeds.
	const bool bMatched = Inspector->BuildAnalysisSampleForTest(Source, Bank->Count, Payload, Scales, View,
		SourceIndices, /*bSkipTombstonedRows*/ false);
	TestTrue(TEXT("negative control: a matching limit is not rejected"), bMatched);

	// Negative control: a SAMPLE build (bSkipTombstonedRows=true, the default) with the
	// same smaller limit is legitimate and must not be rejected -- the guard is scoped to
	// the non-compacting axis only.
	const bool bSampleBuilt = Inspector->BuildAnalysisSampleForTest(Source, 10, Payload, Scales, View,
		SourceIndices, /*bSkipTombstonedRows*/ true);
	TestTrue(TEXT("negative control: a sample build with a smaller limit is not rejected"), bSampleBuilt);

	return true;
}

// Dim 5 (cycle-6 audit S1): the Dot verdict-unavailable status, exercised on a fresh
// Dot asset. The archive-sourced tombstoned-Dot-bank leg (also named in section 25.9)
// is OUT OF SCOPE for slot 3 (no archive source exists yet — slot 4b) and is noted, not
// tested, here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorDotVerdictUnavailableTest,
	"SuperFAISS.D.InspectorDotVerdictUnavailable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorDotVerdictUnavailableTest::RunTest(const FString& Parameters)
{
	USuperFAISSVectorBank* Bank = MakeBank(*this, SeededRows(30, 12, 0xD07), 30, 12,
		ESuperFAISSBankMetric::Dot, ESuperFAISSBankQuantization::Int8);
	if (Bank == nullptr)
	{
		return true;
	}

	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	Inspector->SetBankForTest(Bank);
	Inspector->ProbeNovelty(TEXT("#0"));

	const FSuperFAISSNoveltyResult& Result = Inspector->GetNoveltyResult();
	TestTrue(TEXT("Dot probe: result is valid (a decided status, not silence)"), Result.bValid);
	TestTrue(TEXT("Dot probe: verdict is Unavailable"),
		Result.Verdict == ESuperFAISSNoveltyVerdict::Unavailable);
	TestEqual(TEXT("Dot probe: status text is the documented copy, verbatim"),
		Result.UnavailableStatus, FString(SSuperFAISSBankInspector::DotUnavailableStatus()));
	return true;
}

// Dim 10 (channel-scoped Novelty FEAT leg, "the PANEL-level tri-state at the
// widget"): a Cosine exact-direction twin and an int8 L2
// dequant-identical/byte-different twin, ADAPTED from the core suite
// (`D:\SuperFAISS\tests\test_main.cpp` TestM2NoveltyProbeDistance) — same
// dims=32/channels={{0,16},{16,16}} grid alignment (a past lesson) and the same
// channel-0/channel-1 payload values, but as TWO STORED BANK ROWS rather than one
// stored row plus a hand-built probe vector: View B's probe (section 25.5) always
// self-selects an EXISTING row by id/index — unlike `NoveltyProbeDistance`'s own
// direct two-operand contract — so the widget-level twin needs its "probe" and its
// "stored comparison" both present as real rows in the same bank, row 0 probing row 1.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorNoveltyChannelScopedPanelFeatTest,
	"SuperFAISS.D.InspectorNoveltyChannelScopedPanelFeat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorNoveltyChannelScopedPanelFeatTest::RunTest(const FString& Parameters)
{
	const TArray<FName> ChannelNames = {TEXT("chan0"), TEXT("chan1")};
	const TArray<int32> ChannelOffsets = {0, 16};
	const TArray<int32> ChannelLengths = {16, 16};
	const int32 Dims = 32;

	// Strike 10: both rows share the channel-0 direction (1,0,0,...); channel 1 (out
	// of scope) deliberately differs between them, proving the out-of-scope channel
	// does not affect the scoped verdict.
	{
		TArray<float> Rows;
		Rows.SetNumZeroed(2 * Dims);
		Rows[0] = 1.0f;       // row 0, channel-0 direction (1,0,0,...)
		Rows[16] = 3.0f;      // row 0, channel-1 (out of scope)
		Rows[Dims + 0] = 1.0f; // row 1, SAME channel-0 direction
		Rows[Dims + 16] = 7.0f; // row 1, a DIFFERENT channel-1 value
		USuperFAISSVectorBank* Bank = MakeBank(*this, Rows, 2, Dims,
			ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Int8,
			ChannelNames, ChannelOffsets, ChannelLengths);
		if (Bank != nullptr)
		{
			TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
			Inspector->SetBankForTest(Bank);
			Inspector->SetAnalysisScopeForTest(TEXT("chan0"));
			Inspector->ProbeNovelty(TEXT("#0"));
			TestTrue(TEXT("strike-10 twin: verdict computed under channel scope"),
				Inspector->GetNoveltyResult().bValid);
			TestTrue(TEXT("strike-10 twin: exact-direction slice twin verdicts duplicate"),
				Inspector->GetNoveltyResult().Verdict == ESuperFAISSNoveltyVerdict::Duplicate);
		}
	}
	// Strike 11: both rows share IDENTICAL channel-0 dequantized values (100/127,
	// 50/127) but a DIFFERENT per-row int8 scale, forced by a different channel-1
	// (out-of-scope) magnitude driving each row's own max-abs -- row 0's scale is
	// 2/127 (bytes [50,25,...]), row 1's is 1/127 (bytes [100,50,...]): the exact
	// (bytes, scale) mismatch strike 11 exercised, now as two stored rows instead of
	// one stored row plus a hand-built probe.
	{
		TArray<float> Rows;
		Rows.SetNumZeroed(2 * Dims);
		Rows[0] = 100.0f / 127.0f;
		Rows[1] = 50.0f / 127.0f;
		Rows[16] = 2.0f;
		Rows[Dims + 0] = 100.0f / 127.0f;
		Rows[Dims + 1] = 50.0f / 127.0f;
		Rows[Dims + 16] = 1.0f;
		USuperFAISSVectorBank* Bank = MakeBank(*this, Rows, 2, Dims,
			ESuperFAISSBankMetric::L2, ESuperFAISSBankQuantization::Int8,
			ChannelNames, ChannelOffsets, ChannelLengths);
		if (Bank != nullptr)
		{
			TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
			Inspector->SetBankForTest(Bank);
			Inspector->SetAnalysisScopeForTest(TEXT("chan0"));
			Inspector->ProbeNovelty(TEXT("#0"));
			TestTrue(TEXT("strike-11 twin: verdict computed under channel scope"),
				Inspector->GetNoveltyResult().bValid);
			TestTrue(TEXT("strike-11 twin: kernel-zero L2 twin (different bytes/scale) verdicts duplicate"),
				Inspector->GetNoveltyResult().Verdict == ESuperFAISSNoveltyVerdict::Duplicate);
		}
	}
	return true;
}

// Limb 1 chooses no numeric
// threshold of its own -- the duplicate test is the METRIC'S OWN exact zero, read from
// the metric, never a single epsilon applied uniformly across metrics. A blind review gate
// executed a whole-row float32 Cosine pair (dims 768, one lane of a copy nudged by 1e-6)
// and measured a nonzero residual distance that nonetheless read as `duplicate` under a
// uniform `< 1e-8f` test -- this cell is that fixture, adapted: a tiny-but-nonzero
// near-duplicate must fall through to limb 2 (Familiar/Novel), never verdict Duplicate; a
// true byte-identical duplicate of the same row must still verdict Duplicate (the control
// leg -- proves the fix did not just make every whole-row Cosine probe fall through).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorNoveltyLimb1ExactZeroBoundaryTest,
	"SuperFAISS.D.InspectorNoveltyLimb1ExactZeroBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorNoveltyLimb1ExactZeroBoundaryTest::RunTest(const FString& Parameters)
{
	const int32 Dims = 768;
	const int32 Count = 40;
	TArray<float> Rows = SeededRows(Count, Dims, 0x7686);
	// Row 1: a near-duplicate of row 0 -- byte-different, one lane nudged by a tiny,
	// nonzero amount. Row 2: a byte-identical duplicate of row 0 (the control).
	for (int32 d = 0; d < Dims; ++d)
	{
		Rows[Dims + d] = Rows[d];
	}
	Rows[Dims + 0] += 1e-6f;
	for (int32 d = 0; d < Dims; ++d)
	{
		Rows[2 * Dims + d] = Rows[d];
	}

	USuperFAISSVectorBank* Bank = MakeBank(*this, Rows, Count, Dims,
		ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Float32);
	if (Bank == nullptr)
	{
		return true;
	}

	{
		TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
		Inspector->SetBankForTest(Bank);
		Inspector->ProbeNovelty(TEXT("#1"));
		const FSuperFAISSNoveltyResult& Result = Inspector->GetNoveltyResult();
		TestTrue(TEXT("limb-1 boundary: near-duplicate verdict computed"), Result.bValid);
		TestTrue(TEXT("limb-1 boundary: a tiny-but-nonzero near-duplicate falls through to limb 2, never Duplicate"),
			Result.Verdict != ESuperFAISSNoveltyVerdict::Duplicate);
	}
	{
		TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
		Inspector->SetBankForTest(Bank);
		Inspector->ProbeNovelty(TEXT("#2"));
		const FSuperFAISSNoveltyResult& Result = Inspector->GetNoveltyResult();
		TestTrue(TEXT("limb-1 boundary control: byte-identical duplicate verdict computed"), Result.bValid);
		TestTrue(TEXT("limb-1 boundary control: a byte-identical duplicate still verdicts Duplicate"),
			Result.Verdict == ESuperFAISSNoveltyVerdict::Duplicate);
	}
	return true;
}

// A `duplicate` verdict is decided entirely by limb 1 (the exact-zero metric distance);
// limb 2's CDF statistic (Score/SampledCount/TotalCount) is never consulted for this
// verdict (plan section 25.4) and ProbeNovelty's own `if (Verdict != Duplicate)` guard
// short-circuits limb 2 so those fields stay at their zeroed defaults. Before this fix,
// BuildNoveltyVerdictText() printed them unconditionally regardless of verdict, rendering
// the meaningless "duplicate — novelty 0.0000 vs 0 of 0 sampled rows". This cell asserts
// the RENDERED text a byte-identical-duplicate probe produces does not contain that
// zeroed CDF readout.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorNoveltyDuplicateTextOmitsCdfStatsTest,
	"SuperFAISS.D.InspectorNoveltyDuplicateTextOmitsCdfStats",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorNoveltyDuplicateTextOmitsCdfStatsTest::RunTest(const FString& Parameters)
{
	const int32 Dims = 768;
	const int32 Count = 40;
	TArray<float> Rows = SeededRows(Count, Dims, 0x7686);
	// Row 1: a byte-identical duplicate of row 0 -- forces the Duplicate verdict via
	// limb 1 alone, exactly like the control leg above.
	for (int32 d = 0; d < Dims; ++d)
	{
		Rows[Dims + d] = Rows[d];
	}

	USuperFAISSVectorBank* Bank = MakeBank(*this, Rows, Count, Dims,
		ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Float32);
	if (Bank == nullptr)
	{
		return true;
	}

	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	Inspector->SetBankForTest(Bank);
	Inspector->ProbeNovelty(TEXT("#1"));

	const FSuperFAISSNoveltyResult& Result = Inspector->GetNoveltyResult();
	TestTrue(TEXT("(setup) verdict computed"), Result.bValid);
	if (Result.Verdict != ESuperFAISSNoveltyVerdict::Duplicate)
	{
		AddError(TEXT("(setup) probed row unexpectedly did not verdict Duplicate -- fixture cannot exercise the render path"));
		return true;
	}

	const FString RenderedText = Inspector->BuildNoveltyVerdictText();
	TestTrue(TEXT("rendered duplicate-verdict text starts with the plain verdict word"),
		RenderedText.Equals(TEXT("duplicate")) || RenderedText.StartsWith(TEXT("duplicate")));
	TestFalse(TEXT("rendered duplicate-verdict text must not contain the meaningless zeroed CDF readout (\"vs 0 of 0 sampled rows\")"),
		RenderedText.Contains(TEXT("sampled rows")));
	TestFalse(TEXT("rendered duplicate-verdict text must not contain a novelty score readout"),
		RenderedText.Contains(TEXT("novelty ")));
	return true;
}

// Launch-gate finding S1: FSuperFAISSNoveltyResult::ZeroEnergyExcludedCount is written by
// ProbeNovelty's limb-2 baseline sample but was rendered NOWHERE -- plan section 25.3
// requires the exclusion count disclosed in the status line, and Structure/Correspondence
// already disclose it. This cell asserts the RENDERED verdict text (BuildNoveltyVerdictText,
// the same function the panel's Text_Lambda calls), not merely that the struct field was
// assigned -- a cell that only checked the field would stay green even if the render path
// never read it. Fixture: a channel-scoped Cosine bank where 5 of 40 rows have zero energy
// in the scoped channel (all-zero over that slice) -- excluded from the limb-2 baseline
// sample, never from the bank itself.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorNoveltyZeroEnergyExcludedRenderedTest,
	"SuperFAISS.D.InspectorNoveltyZeroEnergyExcludedRendered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorNoveltyZeroEnergyExcludedRenderedTest::RunTest(const FString& Parameters)
{
	const TArray<FName> ChannelNames = {TEXT("chan0"), TEXT("chan1")};
	const TArray<int32> ChannelOffsets = {0, 16};
	const TArray<int32> ChannelLengths = {16, 16};
	const int32 Dims = 32;
	const int32 Count = 40;

	TArray<float> Rows = SeededRows(Count, Dims, 0x5E20);
	// Rows 0..4: zero energy in the scoped channel (chan0) -- excluded from the limb-2
	// baseline sample when probing under that scope.
	for (int32 Row = 0; Row < 5; ++Row)
	{
		for (int32 d = 0; d < 16; ++d)
		{
			Rows[Row * Dims + d] = 0.0f;
		}
	}

	USuperFAISSVectorBank* Bank = MakeBank(*this, Rows, Count, Dims,
		ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Float32,
		ChannelNames, ChannelOffsets, ChannelLengths);
	if (Bank == nullptr)
	{
		return true;
	}

	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	Inspector->SetBankForTest(Bank);
	Inspector->SetAnalysisScopeForTest(TEXT("chan0"));
	// Probe a row outside the zero-energy set.
	Inspector->ProbeNovelty(TEXT("#20"));

	const FSuperFAISSNoveltyResult& Result = Inspector->GetNoveltyResult();
	TestTrue(TEXT("(setup) verdict computed"), Result.bValid);
	if (Result.Verdict == ESuperFAISSNoveltyVerdict::Duplicate)
	{
		AddError(TEXT("(setup) probed row unexpectedly verdicts Duplicate -- fixture cannot exercise the exclusion disclosure"));
		return true;
	}
	TestTrue(TEXT("(setup) the baseline sample actually excluded zero-energy rows"),
		Result.ZeroEnergyExcludedCount > 0);

	const FString RenderedText = Inspector->BuildNoveltyVerdictText();
	TestTrue(TEXT("rendered novelty text discloses the zero-energy exclusion count"),
		RenderedText.Contains(FString::Printf(TEXT("%d excluded (zero energy in channel)"),
			Result.ZeroEnergyExcludedCount)));
	return true;
}

// Dim 7: self-exclusion on row probes (a probed row never appears in its own evidence
// list); the low-confidence marking below the effective-sample floor (64 rows,
// section 25.5) — scoped to limb-2 verdicts only, never to `duplicate`.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorNoveltySelfExclusionLowConfidenceTest,
	"SuperFAISS.D.InspectorNoveltySelfExclusionLowConfidence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorNoveltySelfExclusionLowConfidenceTest::RunTest(const FString& Parameters)
{
	// Below the 64-row low-confidence floor by construction.
	USuperFAISSVectorBank* SmallBank = MakeBank(*this, SeededRows(30, 12, 0x50A1), 30, 12,
		ESuperFAISSBankMetric::L2, ESuperFAISSBankQuantization::Float32);
	if (SmallBank == nullptr)
	{
		return true;
	}
	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	Inspector->SetBankForTest(SmallBank);
	Inspector->ProbeNovelty(TEXT("#5"));

	const FSuperFAISSNoveltyResult& Result = Inspector->GetNoveltyResult();
	TestTrue(TEXT("low-confidence: verdict computed on a sub-floor bank"), Result.bValid);
	if (Result.Verdict != ESuperFAISSNoveltyVerdict::Duplicate)
	{
		TestTrue(TEXT("low-confidence: a limb-2 verdict under 64 rows carries the marking"),
			Result.bLowConfidence);
	}

	// Self-exclusion: probing row 5 must never list row 5 in its own evidence. GREEN
	// AT AUTHORING TIME as written — the scaffold's ProbeNovelty poison leaves
	// NoveltyEvidenceLines empty (evidence population is not yet built, not scaffolded
	// with a poison the way Structure/the verdict are, since a poison evidence line
	// would have to either fabricate row identity — undermining the very property
	// this cell checks — or say nothing, which is what an empty list already does).
	// Kept as a standing regression guard: it becomes a genuine red/green check the
	// moment evidence population is wired.
	bool bFoundSelf = false;
	for (const TSharedPtr<FString>& Line : Inspector->GetNoveltyEvidenceLines())
	{
		if (Line.IsValid() && Line->Contains(TEXT("#5"))) { bFoundSelf = true; }
	}
	TestFalse(TEXT("self-exclusion: probed row never appears in its own evidence list"), bFoundSelf);
	return true;
}

// Dim 1: switching the analysis scope, or switching to a Dot-metric bank, both clear a
// prior verdict -- "the reset matrix covers the scope axis, not only the metric axis"
// (section 25.5).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorNoveltyResetOnScopeMetricChangeTest,
	"SuperFAISS.D.InspectorNoveltyResetOnScopeMetricChange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorNoveltyResetOnScopeMetricChangeTest::RunTest(const FString& Parameters)
{
	const TArray<FName> ChannelNames = {TEXT("chanA"), TEXT("chanB")};
	const TArray<int32> ChannelOffsets = {0, 16};
	const TArray<int32> ChannelLengths = {16, 16};
	USuperFAISSVectorBank* CosineBank = MakeBank(*this, SeededRows(40, 32, 0x5C0E), 40, 32,
		ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Int8,
		ChannelNames, ChannelOffsets, ChannelLengths);
	USuperFAISSVectorBank* DotBank = MakeBank(*this, SeededRows(40, 32, 0x5C0F), 40, 32,
		ESuperFAISSBankMetric::Dot, ESuperFAISSBankQuantization::Int8);
	if (CosineBank == nullptr || DotBank == nullptr)
	{
		return true;
	}

	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	Inspector->SetBankForTest(CosineBank);
	Inspector->ProbeNovelty(TEXT("#0"));
	TestTrue(TEXT("(setup) a verdict was computed before the scope change"),
		Inspector->GetNoveltyResult().bValid);

	Inspector->SetAnalysisScopeForTest(TEXT("chanA"));
	TestFalse(TEXT("scope change clears the prior verdict"), Inspector->GetNoveltyResult().bValid);

	Inspector->ProbeNovelty(TEXT("#0"));
	TestTrue(TEXT("(setup) a verdict was computed before the metric change"),
		Inspector->GetNoveltyResult().bValid);
	Inspector->SetBankForTest(DotBank);
	TestFalse(TEXT("metric change (switch to a Dot bank) clears the prior verdict"),
		Inspector->GetNoveltyResult().bValid);
	return true;
}

// Re-gate F7: the evidence query is recomputed per probe, never cached — probing row A,
// then row B, then row A again must produce the SAME evidence for row A both times, not
// a stale copy contaminated by row B's intervening probe. GREEN AT AUTHORING TIME — the
// scaffold's ProbeNovelty poison never populates NoveltyEvidenceLines at all (see the
// self-exclusion cell's own note), so "nothing cached" is vacuously true of a stub that
// caches nothing. Kept as a standing regression guard against a FUTURE caching bug,
// exactly like the concurrency grep-target cell below.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorNoveltyEvidenceRecomputedTest,
	"SuperFAISS.D.InspectorNoveltyEvidenceRecomputed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorNoveltyEvidenceRecomputedTest::RunTest(const FString& Parameters)
{
	USuperFAISSVectorBank* Bank = MakeBank(*this, SeededRows(50, 16, 0xE71D), 50, 16,
		ESuperFAISSBankMetric::L2, ESuperFAISSBankQuantization::Float32);
	if (Bank == nullptr)
	{
		return true;
	}
	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	Inspector->SetBankForTest(Bank);

	Inspector->ProbeNovelty(TEXT("#3"));
	TArray<FString> FirstEvidence;
	for (const TSharedPtr<FString>& Line : Inspector->GetNoveltyEvidenceLines())
	{
		if (Line.IsValid()) { FirstEvidence.Add(*Line); }
	}

	Inspector->ProbeNovelty(TEXT("#17"));

	Inspector->ProbeNovelty(TEXT("#3"));
	TArray<FString> ThirdEvidence;
	for (const TSharedPtr<FString>& Line : Inspector->GetNoveltyEvidenceLines())
	{
		if (Line.IsValid()) { ThirdEvidence.Add(*Line); }
	}
	TestTrue(TEXT("evidence recomputed per probe: row #3 probed twice yields identical evidence"),
		FirstEvidence == ThirdEvidence);
	return true;
}

// Dim 3 (applicable narrowly for M4): the absence claim, held by a mechanical oracle
// rather than a review impression (audit N-5) — no `QueryAsync` / `FSuperFAISSTicket`
// symbol appears in the Inspector's translation units, which is what makes the B4
// async-lifetime hazard class inapplicable by construction. GREEN AT AUTHORING TIME —
// this round's scaffold introduces no async dispatch, so the negative already holds;
// the cell's value is as a STANDING regression guard for every future edit to these
// files, exactly as the design's own "checked the same way the
// section 5.1 no-aliasing code rule is" describes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorConcurrencyGrepTargetTest,
	"SuperFAISS.D.InspectorConcurrencyGrepTarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorConcurrencyGrepTargetTest::RunTest(const FString& Parameters)
{
	const FString ModuleDir = FPaths::Combine(
		IPluginManager::Get().FindPlugin(TEXT("SuperFAISSUnreal"))->GetBaseDir(),
		TEXT("Source/SuperFAISSUnrealEditor"));
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *ModuleDir, TEXT("*.cpp"), true, false, false);
	IFileManager::Get().FindFilesRecursive(Files, *ModuleDir, TEXT("*.h"), true, false, false);

	const TCHAR* BannedSymbols[] = {TEXT("QueryAsync"), TEXT("FSuperFAISSTicket"), TEXT("AsyncTask(")};
	for (const FString& File : Files)
	{
		// The claim is about PRODUCTION code. Test sources under Tests/ legitimately
		// name these symbols as strings (this very file's BannedSymbols array and
		// doc comments) in order to check for their absence elsewhere -- scanning
		// this file's own text would self-match and is excluded, matching how the
		// section 5.1 no-aliasing code rule this cell is modeled on scans shippable
		// source, not the harness that checks it.
		if (File.Contains(TEXT("/Tests/")) || File.Contains(TEXT("\\Tests\\"))) { continue; }
		FString Contents;
		if (!FFileHelper::LoadFileToString(Contents, *File)) { continue; }
		for (const TCHAR* Symbol : BannedSymbols)
		{
			TestFalse(FString::Printf(TEXT("%s must not appear in %s"), Symbol, *File),
				Contents.Contains(Symbol));
		}
	}
	return true;
}

// ===========================================================================
// View C (Correspondence), slot 4 (section 25.5; Coverage Model section 25.9's M4
// dimension, scoped to slot 4 — the archive/tombstone legs remain out of scope, slot 4b).
// ===========================================================================

// Dim 2: the second-bank compatibility rejection matrix (dims mismatch, metric mismatch,
// invalid asset) + the "no second bank selected" precondition. GREEN AT AUTHORING TIME —
// CheckSecondBankCompatible is real, shipped-shape logic (plain field comparisons
// already available on USuperFAISSVectorBank), no achievement left to gate; the file
// header's disclosure discipline applies (mirrors slot 3's self-exclusion/
// evidence-recompute/disclosure-copy/persistence cells). Kept as a standing regression
// guard, not a red-unimplemented cell.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorCorrespondenceCompatibilityMatrixTest,
	"SuperFAISS.D.InspectorCorrespondenceCompatibilityMatrix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorCorrespondenceCompatibilityMatrixTest::RunTest(const FString& Parameters)
{
	USuperFAISSVectorBank* Primary = MakeBank(*this, SeededRows(20, 8, 0xC0A1), 20, 8,
		ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Float32);
	if (Primary == nullptr)
	{
		return true;
	}
	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	Inspector->SetBankForTest(Primary);

	// No second bank selected at all.
	Inspector->ComputeCorrespondence();
	TestEqual(TEXT("no second bank: status"), Inspector->GetCorrespondenceStatus(),
		FString(TEXT("no second bank selected")));
	TestEqual(TEXT("no second bank: pair list empty"), Inspector->GetMatchPairResults().Num(), 0);

	// Invalid asset: a NewObject with no InitFromSource ever called fails IsValid().
	USuperFAISSVectorBank* InvalidBank = NewObject<USuperFAISSVectorBank>();
	Inspector->SetSecondBankForTest(InvalidBank);
	Inspector->ComputeCorrespondence();
	TestEqual(TEXT("invalid asset: status"), Inspector->GetCorrespondenceStatus(),
		FString(TEXT("second bank: invalid asset")));
	TestEqual(TEXT("invalid asset: pair list empty"), Inspector->GetMatchPairResults().Num(), 0);

	// Dims mismatch: same metric, different dims.
	USuperFAISSVectorBank* WrongDims = MakeBank(*this, SeededRows(20, 12, 0xC0A2), 20, 12,
		ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Float32);
	if (WrongDims != nullptr)
	{
		Inspector->SetSecondBankForTest(WrongDims);
		Inspector->ComputeCorrespondence();
		TestEqual(TEXT("dims mismatch: status"), Inspector->GetCorrespondenceStatus(),
			FString(TEXT("second bank: dims mismatch")));
		TestEqual(TEXT("dims mismatch: pair list empty"), Inspector->GetMatchPairResults().Num(), 0);
	}

	// Metric mismatch: same dims, different metric.
	USuperFAISSVectorBank* WrongMetric = MakeBank(*this, SeededRows(20, 8, 0xC0A3), 20, 8,
		ESuperFAISSBankMetric::L2, ESuperFAISSBankQuantization::Float32);
	if (WrongMetric != nullptr)
	{
		Inspector->SetSecondBankForTest(WrongMetric);
		Inspector->ComputeCorrespondence();
		TestEqual(TEXT("metric mismatch: status"), Inspector->GetCorrespondenceStatus(),
			FString(TEXT("second bank: metric mismatch")));
		TestEqual(TEXT("metric mismatch: pair list empty"), Inspector->GetMatchPairResults().Num(), 0);
	}
	return true;
}

// Dim 5 (audit N-3, the late-rejection UI contract): an incompatible pair rejected at
// compute clears any prior correspondence list to empty — a stale pair list from an
// earlier VALID pair is never left rendering beside the rejection status. GREEN AT
// AUTHORING TIME — ComputeCorrespondence() Resets MatchPairResults unconditionally at
// the top of the function, before any compatibility check runs, so this negative claim
// already holds of the scaffold (mirrors the file header's disclosure discipline: the
// claim is meaningful once a real, non-empty pair list exists to leak, which the
// compatible-pair poison path already provides). Kept as a standing regression guard.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorCorrespondenceLateRejectionClearsListTest,
	"SuperFAISS.D.InspectorCorrespondenceLateRejectionClearsList",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorCorrespondenceLateRejectionClearsListTest::RunTest(const FString& Parameters)
{
	USuperFAISSVectorBank* Primary = MakeBank(*this, SeededRows(20, 8, 0xC0B1), 20, 8,
		ESuperFAISSBankMetric::L2, ESuperFAISSBankQuantization::Float32);
	USuperFAISSVectorBank* CompatibleSecond = MakeBank(*this, SeededRows(20, 8, 0xC0B2), 20, 8,
		ESuperFAISSBankMetric::L2, ESuperFAISSBankQuantization::Float32);
	if (Primary == nullptr || CompatibleSecond == nullptr)
	{
		return true;
	}
	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	Inspector->SetBankForTest(Primary);
	Inspector->SetSecondBankForTest(CompatibleSecond);
	Inspector->ComputeCorrespondence();
	TestTrue(TEXT("(setup) a pair list exists before the incompatible swap"),
		Inspector->GetMatchPairResults().Num() > 0);

	USuperFAISSVectorBank* IncompatibleSecond = MakeBank(*this, SeededRows(20, 12, 0xC0B3), 20, 12,
		ESuperFAISSBankMetric::L2, ESuperFAISSBankQuantization::Float32);
	if (IncompatibleSecond == nullptr)
	{
		return true;
	}
	Inspector->SetSecondBankForTest(IncompatibleSecond);
	Inspector->ComputeCorrespondence();
	TestEqual(TEXT("late rejection: prior pair list cleared to empty"),
		Inspector->GetMatchPairResults().Num(), 0);
	TestEqual(TEXT("late rejection: status is the dims-mismatch line item"),
		Inspector->GetCorrespondenceStatus(), FString(TEXT("second bank: dims mismatch")));
	return true;
}

// Dim 8 (composition): correspondence x quantization mix — Quantization MAY differ
// across the pair (E-D1-3), disclosed in the status line rather than rejected. Pinned
// contract (ComputeCorrespondence()'s own doc comment, this round's forced reading): the
// status appends ", mixed quantization" when Quantization differs.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorCorrespondenceMixedQuantizationDisclosureTest,
	"SuperFAISS.D.InspectorCorrespondenceMixedQuantizationDisclosure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorCorrespondenceMixedQuantizationDisclosureTest::RunTest(const FString& Parameters)
{
	USuperFAISSVectorBank* Primary = MakeBank(*this, SeededRows(20, 8, 0xC0C1), 20, 8,
		ESuperFAISSBankMetric::L2, ESuperFAISSBankQuantization::Int8);
	USuperFAISSVectorBank* Second = MakeBank(*this, SeededRows(20, 8, 0xC0C2), 20, 8,
		ESuperFAISSBankMetric::L2, ESuperFAISSBankQuantization::Float32);
	if (Primary == nullptr || Second == nullptr)
	{
		return true;
	}
	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	Inspector->SetBankForTest(Primary);
	Inspector->SetSecondBankForTest(Second);
	Inspector->ComputeCorrespondence();
	TestTrue(TEXT("mixed quantization: a mixed int8/float32 pair is NOT rejected"),
		Inspector->GetMatchPairResults().Num() > 0);
	TestTrue(TEXT("mixed quantization: disclosed in the status line"),
		Inspector->GetCorrespondenceStatus().Contains(TEXT("mixed quantization")));
	return true;
}

// Dim 1, the full M4 invalidation matrix WITHOUT the archive-swap leg (section 25.9's
// "primary re-select, scope change, second-bank change, parameter change" -- the fourth,
// parameter change, composes no observable state to construct a cell against: see
// InvalidateAnalysisCaches()'s own header comment, ComputeCorrespondence reads
// MatchK/CslsMarginThreshold fresh on every trigger click, mirroring Structure's
// no-persistent-cache posture -- noted, not celled, exactly Novelty-baseline's F1
// precedent shape in reverse). Primary re-select and scope change are GREEN AT
// AUTHORING TIME (InvalidateAnalysisCaches() already really resets MatchPairResults,
// extending its already-shipped slot-3 body). The second-bank-change leg is
// red-unimplemented: OnSecondBankSelected() is a no-op stub this round.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorCorrespondenceInvalidationMatrixTest,
	"SuperFAISS.D.InspectorCorrespondenceInvalidationMatrix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorCorrespondenceInvalidationMatrixTest::RunTest(const FString& Parameters)
{
	const TArray<FName> ChannelNames = {TEXT("chanA"), TEXT("chanB")};
	const TArray<int32> ChannelOffsets = {0, 16};
	const TArray<int32> ChannelLengths = {16, 16};
	USuperFAISSVectorBank* Primary = MakeBank(*this, SeededRows(20, 32, 0xC0D1), 20, 32,
		ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Int8,
		ChannelNames, ChannelOffsets, ChannelLengths);
	USuperFAISSVectorBank* SecondA = MakeBank(*this, SeededRows(20, 32, 0xC0D2), 20, 32,
		ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Int8);
	USuperFAISSVectorBank* SecondB = MakeBank(*this, SeededRows(20, 32, 0xC0D3), 20, 32,
		ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Int8);
	if (Primary == nullptr || SecondA == nullptr || SecondB == nullptr)
	{
		return true;
	}
	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);

	// Leg 1: primary re-select.
	Inspector->SetBankForTest(Primary);
	Inspector->SetSecondBankForTest(SecondA);
	Inspector->ComputeCorrespondence();
	TestTrue(TEXT("(setup) a pair list exists before primary re-select"),
		Inspector->GetMatchPairResults().Num() > 0);
	Inspector->SetBankForTest(Primary);
	TestEqual(TEXT("primary re-select clears the Correspondence cache"),
		Inspector->GetMatchPairResults().Num(), 0);

	// Leg 2: analysis-scope change.
	Inspector->SetSecondBankForTest(SecondA);
	Inspector->ComputeCorrespondence();
	TestTrue(TEXT("(setup) a pair list exists before the scope change"),
		Inspector->GetMatchPairResults().Num() > 0);
	Inspector->SetAnalysisScopeForTest(TEXT("chanA"));
	TestEqual(TEXT("scope change clears the Correspondence cache"),
		Inspector->GetMatchPairResults().Num(), 0);
	Inspector->SetAnalysisScopeForTest(TEXT("(whole row)"));

	// Leg 3 (NEW this round): second-bank change.
	Inspector->SetSecondBankForTest(SecondA);
	Inspector->ComputeCorrespondence();
	TestTrue(TEXT("(setup) a pair list exists before the second-bank change"),
		Inspector->GetMatchPairResults().Num() > 0);
	Inspector->SetSecondBankForTest(SecondB);
	TestEqual(TEXT("second-bank change clears the Correspondence cache"),
		Inspector->GetMatchPairResults().Num(), 0);
	return true;
}

// Dim 4, the heavy pass's three size axes (A's SampleLimit cap x A's count x B's count),
// non-archive: cap >= A's count (sample = all of A) at three A/B size relationships,
// including both extreme-asymmetry directions. Wiring-sanity claim (the widget checks
// EXACTLY min(SampleLimit, A.Count) rows regardless of how A and B's sizes relate to
// each other), not a re-derivation of numeric correctness (already proven core-side,
// M3 TestM3MutualNearestMatchesCorrectness across dims x quant x metric). The default
// SampleLimit is 2048; every A.Count below is comfortably under it, so the cap never
// engages here (the cap-engaging case is the crux Panel FEAT below).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorCorrespondenceSizeAxesTest,
	"SuperFAISS.D.InspectorCorrespondenceSizeAxes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorCorrespondenceSizeAxesTest::RunTest(const FString& Parameters)
{
	struct FCase { int32 ACount; int32 BCount; const TCHAR* Name; };
	const FCase Cases[] = {
		{50, 60, TEXT("under-cap (both moderate)")},
		{15, 300, TEXT("tiny A vs large B")},
		{300, 12, TEXT("large A (still under cap) vs tiny B")},
	};
	int32 Seed = 0xC0E0;
	for (const FCase& Case : Cases)
	{
		USuperFAISSVectorBank* A = MakeBank(*this, SeededRows(Case.ACount, 8, Seed++), Case.ACount, 8,
			ESuperFAISSBankMetric::L2, ESuperFAISSBankQuantization::Float32);
		USuperFAISSVectorBank* B = MakeBank(*this, SeededRows(Case.BCount, 8, Seed++), Case.BCount, 8,
			ESuperFAISSBankMetric::L2, ESuperFAISSBankQuantization::Float32);
		if (A == nullptr || B == nullptr)
		{
			continue;
		}
		TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
		Inspector->SetBankForTest(A);
		Inspector->SetSecondBankForTest(B);
		Inspector->ComputeCorrespondence();
		TestEqual(FString::Printf(TEXT("size axes (%s): checked count == A.Count (cap doesn't engage)"),
			Case.Name), Inspector->GetMatchPairResults().Num(), Case.ACount);
	}
	return true;
}

// Dim 5 (the cancel path is first-class) for View C's chunked pass. GREEN AT
// AUTHORING TIME — SuperFAISSInspectorSlowTask::RunChunked is already real, and
// ComputeCorrespondence()'s cancel handling (MatchPairResults stays at its top-of-function
// Reset(), status = "cancelled") is real code this round, not scaffolded. Kept as a
// standing regression guard for THIS caller's own wiring into already-proven
// infrastructure — a builder could plausibly forget to check ChunkResult.bCancelled or
// write partial state before checking it, which is exactly what this cell would catch.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorCorrespondenceCancelTest,
	"SuperFAISS.D.InspectorCorrespondenceCancel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorCorrespondenceCancelTest::RunTest(const FString& Parameters)
{
	USuperFAISSVectorBank* Primary = MakeBank(*this, SeededRows(20, 8, 0xC0F1), 20, 8,
		ESuperFAISSBankMetric::L2, ESuperFAISSBankQuantization::Float32);
	USuperFAISSVectorBank* Second = MakeBank(*this, SeededRows(20, 8, 0xC0F2), 20, 8,
		ESuperFAISSBankMetric::L2, ESuperFAISSBankQuantization::Float32);
	if (Primary == nullptr || Second == nullptr)
	{
		return true;
	}
	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	Inspector->SetBankForTest(Primary);
	Inspector->SetSecondBankForTest(Second);

	// Cancel before the first (and only) chunk boundary.
	Inspector->DebugCancelAfterChunks = 0;
	Inspector->ComputeCorrespondence();
	TestEqual(TEXT("cancel: pair list stays empty (no partial commit)"),
		Inspector->GetMatchPairResults().Num(), 0);
	TestEqual(TEXT("cancel: status is 'cancelled'"), Inspector->GetCorrespondenceStatus(),
		FString(TEXT("cancelled")));

	// A subsequent, never-cancelled run completes normally.
	Inspector->DebugCancelAfterChunks = -1;
	Inspector->ComputeCorrespondence();
	TestTrue(TEXT("post-cancel: a subsequent full run completes (pair list populated)"),
		Inspector->GetMatchPairResults().Num() > 0);
	TestNotEqual(TEXT("post-cancel: status is not 'cancelled'"), Inspector->GetCorrespondenceStatus(),
		FString(TEXT("cancelled")));
	return true;
}

// Dim 10 (the crux): the Correspondence Panel FEAT, PORTED from the
// core-level permutation fixture (TestM3CorrespondencePermutationFeat,
// D:\SuperFAISS\tests\test_main.cpp) and re-shaped for the widget's own sample cap
// (fixture-engagement law: sized ABOVE SampleLimit=2048, not the core fixture's
// hand-picked small sample). The core fixture's one-hot-per-landmark geometry (dims ==
// landmark count) does not scale to 2048+ rows without an O(cap x B.count x dims) cost
// this suite cannot afford (dims would also exceed 2000+); this round's geometry
// achieves the SAME "well-separated by construction" property with dims=8 instead, via
// a grouped one-hot code: landmark index i decomposes as (group = i / 300, slot = i %
// 300) across 8 groups of 300 slots (2400 landmarks total); its row is zero except
// axis[group] = 1000.0 + 10.0*slot. Separation, by elementary arithmetic (stated, not
// executed brute-force -- the "trace the mechanism" bar is met here
// by the construction's own arithmetic, which is a single-term additive scheme, not the
// core fixture's more delicate magnitude-proximity reasoning that produced a past
// fracture): same-group distinct-slot L2 distance is exactly |10*(slotA-slotB)| >= 10;
// cross-group distance is sqrt(magA^2 + magB^2) with both magnitudes in [1000, 4010],
// so >= sqrt(1000^2+1000^2) ~= 1414 -- zero is the unique, strict global minimum for
// every landmark's true partner, by construction. B is A's landmarks in REVERSED order
// (bit-identical rows, permuted slot order) -- landmark i's true B partner sits at
// whichever B position holds that same vector, recovered by asserting SourceIndexB ==
// 2399 - SourceIndexA (the reversal), not by knowing the position in advance. Two
// A-only noise rows (axis 7, magnitudes 3995/4000 -- just past axis 7's own landmark
// range [1000,3990]) and two B-only noise rows (axis 0, magnitudes 990/995 -- just
// before axis 0's own range) mirror the core fixture's "anchor near an existing axis,
// not a small unrelated magnitude" fix (a documented past bug) -- a noise
// row's nearest-in-the-other-bank is its own axis's REAL landmark (tiny same-axis gap),
// whose own back-verification prefers ITSELF (distance 0) over the noise (distance 5 or
// 10), so noise correctly reports unmatched without any risk of the cross-noise
// proximity bug the core fixture's comment documents.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorCorrespondencePanelFeatTest,
	"SuperFAISS.D.InspectorCorrespondencePanelFeat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorCorrespondencePanelFeatTest::RunTest(const FString& Parameters)
{
	const int32 Dims = 8;
	const int32 Groups = 8;
	const int32 SlotsPerGroup = 300;
	const int32 LandmarkCount = Groups * SlotsPerGroup; // 2400 -- exceeds SampleLimit=2048

	auto MakeLandmarkRow = [Dims](int32 Group, float Magnitude) -> TArray<float>
	{
		TArray<float> Row;
		Row.SetNumZeroed(Dims);
		Row[Group] = Magnitude;
		return Row;
	};

	TArray<float> ARows;
	ARows.Reserve((LandmarkCount + 2) * Dims);
	for (int32 i = 0; i < LandmarkCount; ++i)
	{
		const int32 Group = i / SlotsPerGroup;
		const int32 Slot = i % SlotsPerGroup;
		ARows.Append(MakeLandmarkRow(Group, 1000.0f + 10.0f * static_cast<float>(Slot)));
	}
	const int32 ANoise0 = LandmarkCount, ANoise1 = LandmarkCount + 1;
	ARows.Append(MakeLandmarkRow(7, 3995.0f));
	ARows.Append(MakeLandmarkRow(7, 4000.0f));
	const int32 ACount = LandmarkCount + 2;

	TArray<float> BRows;
	BRows.Reserve((LandmarkCount + 2) * Dims);
	for (int32 i = 0; i < LandmarkCount; ++i)
	{
		const int32 Landmark = LandmarkCount - 1 - i; // reversed permutation
		const int32 Group = Landmark / SlotsPerGroup;
		const int32 Slot = Landmark % SlotsPerGroup;
		BRows.Append(MakeLandmarkRow(Group, 1000.0f + 10.0f * static_cast<float>(Slot)));
	}
	BRows.Append(MakeLandmarkRow(0, 990.0f));
	BRows.Append(MakeLandmarkRow(0, 995.0f));
	const int32 BCount = LandmarkCount + 2;

	USuperFAISSVectorBank* A = MakeBank(*this, ARows, ACount, Dims,
		ESuperFAISSBankMetric::L2, ESuperFAISSBankQuantization::Float32);
	USuperFAISSVectorBank* B = MakeBank(*this, BRows, BCount, Dims,
		ESuperFAISSBankMetric::L2, ESuperFAISSBankQuantization::Float32);
	if (A == nullptr || B == nullptr)
	{
		return true;
	}

	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	Inspector->SetBankForTest(A);
	Inspector->SetSecondBankForTest(B);
	Inspector->ComputeCorrespondence();

	const TArray<FSuperFAISSMatchPairResult>& Pairs = Inspector->GetMatchPairResults();
	TestEqual(TEXT("Panel FEAT: checked count == SampleLimit (the cap engages, 2400 > 2048)"),
		Pairs.Num(), 2048);

	int32 Recovered = 0, Spurious = 0, NoiseUnmatched = 0, UnexpectedNoiseState = 0;
	for (const FSuperFAISSMatchPairResult& Pair : Pairs)
	{
		if (Pair.SourceIndexA < LandmarkCount)
		{
			const int32 ExpectB = LandmarkCount - 1 - Pair.SourceIndexA;
			if (Pair.State == ESuperFAISSMatchState::Matched && Pair.SourceIndexB == ExpectB)
			{
				++Recovered;
			}
			else
			{
				++Spurious;
			}
		}
		else // one of the two A-only noise rows, if the deterministic stride happened to sample it
		{
			if (Pair.State == ESuperFAISSMatchState::Unmatched)
			{
				++NoiseUnmatched;
			}
			else
			{
				++UnexpectedNoiseState;
			}
		}
	}
	TestEqual(TEXT("Panel FEAT: every checked landmark's true reversed partner recovered"),
		Recovered, Pairs.Num() - (NoiseUnmatched + UnexpectedNoiseState));
	TestEqual(TEXT("Panel FEAT: zero spurious matches"), Spurious, 0);
	TestEqual(TEXT("Panel FEAT: any sampled noise row reports unmatched (never a false match)"),
		UnexpectedNoiseState, 0);

	// The coverage line, verbatim (dim 10, "coverage line reads `N of M A-rows checked`").
	TestTrue(TEXT("Panel FEAT: status line carries the verbatim 'N of M A-rows checked' coverage line"),
		Inspector->GetCorrespondenceStatus().Contains(
			FString::Printf(TEXT("%d of %d A-rows checked"), 2048, ACount)));
	return true;
}

// ===========================================================================
// The inspection-source abstraction. Coverage Model's
// ARCHIVE-related cells, deferred out of the earlier passes per their own
// test-design artifacts' scope sections.
// ===========================================================================

// Dim 2/5 (the archive rejection matrix): a truncated archive, a wrong-version archive,
// and a non-archive file each fail through core Load's rejection with ITS OWN status --
// singular, per the FORCED READING this round states (section 4 of the test-design
// artifact): core ScratchBank::Load collapses every format-level failure into ONE
// Status::BadFormat value (confirmed by reading scratch.cpp -- no VersionMismatch/
// CorruptData status exists), so the three named failure KINDS are three DIFFERENT
// FIXTURES exercising the SAME one status text, not three distinct strings. GREEN AT
// AUTHORING TIME for the rejection legs -- OpenScratchArchiveFromBytes is real, mechanical
// wiring of an already-real, already-shipped core primitive (LoadFromBytes -> core Load,
// reject-over-degrade); no achievement was scaffolded to fake red here (the
// discipline: a cell that cannot be made red by a stub is disclosed, not faked). Also
// covers the success leg: a well-formed archive opens, becomes the primary source, and
// supersedes the asset-registry selection.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorArchiveOpenRejectionMatrixTest,
	"SuperFAISS.D.InspectorArchiveOpenRejectionMatrix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorArchiveOpenRejectionMatrixTest::RunTest(const FString& Parameters)
{
	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);

	// Establish a real "previously open" asset selection before every rejection attempt,
	// so "widget state unchanged" is a meaningful claim, not vacuously true of an
	// already-empty selection.
	USuperFAISSVectorBank* Baseline = MakeBank(*this, SeededRows(10, 4, 0xA001), 10, 4,
		ESuperFAISSBankMetric::L2, ESuperFAISSBankQuantization::Float32);
	if (Baseline == nullptr)
	{
		return true;
	}
	Inspector->SetBankForTest(Baseline);
	TestEqual(TEXT("(setup) primary source is the asset before any archive attempt"),
		static_cast<uint8>(Inspector->GetPrimarySource().Kind), static_cast<uint8>(FSuperFAISSInspectionSource::EKind::Asset));

	// A well-formed archive (the positive control every rejection fixture is derived by
	// corrupting).
	TArray<float> Rows = SeededRows(20, 6, 0xA002);
	TArray<uint8> ValidBytes;
	if (!MakeScratchArchiveBytes(*this, Rows, 20, 6, ESuperFAISSBankMetric::L2,
		ESuperFAISSBankQuantization::Float32, {}, ValidBytes))
	{
		return true;
	}
	TestTrue(TEXT("valid bytes are non-trivially sized"), ValidBytes.Num() > 32);

	// Truncated: keep less than even the 32-byte header.
	{
		TArray<uint8> Truncated = ValidBytes;
		Truncated.SetNum(FMath::Min(10, ValidBytes.Num()));
		const bool bOpened = Inspector->OpenScratchArchiveFromBytes(Truncated, TEXT("truncated.bin"));
		TestFalse(TEXT("truncated archive: Open reports failure"), bOpened);
		TestTrue(TEXT("truncated archive: status line carries a bad-format rejection"),
			Inspector->GetArchiveOpenStatus().Contains(TEXT("bad format")));
		TestEqual(TEXT("truncated archive: primary source unchanged (still the asset)"),
			static_cast<uint8>(Inspector->GetPrimarySource().Kind), static_cast<uint8>(FSuperFAISSInspectionSource::EKind::Asset));
	}

	// Wrong version: the header's version field (bytes [4,8), little-endian) tampered
	// past the documented [1,3] range (scratch.cpp: "Version 0 or > 3 is a hard reject").
	{
		TArray<uint8> WrongVersion = ValidBytes;
		if (WrongVersion.Num() > 7)
		{
			WrongVersion[4] = 200;
			WrongVersion[5] = 0;
			WrongVersion[6] = 0;
			WrongVersion[7] = 0;
		}
		const bool bOpened = Inspector->OpenScratchArchiveFromBytes(WrongVersion, TEXT("wrong-version.bin"));
		TestFalse(TEXT("wrong-version archive: Open reports failure"), bOpened);
		TestTrue(TEXT("wrong-version archive: status line carries a bad-format rejection"),
			Inspector->GetArchiveOpenStatus().Contains(TEXT("bad format")));
		TestEqual(TEXT("wrong-version archive: primary source unchanged (still the asset)"),
			static_cast<uint8>(Inspector->GetPrimarySource().Kind), static_cast<uint8>(FSuperFAISSInspectionSource::EKind::Asset));
	}

	// Non-archive file: bytes that never carry the magic at all.
	{
		TArray<uint8> Garbage;
		Garbage.SetNumZeroed(96);
		for (int32 i = 0; i < Garbage.Num(); ++i)
		{
			Garbage[i] = static_cast<uint8>((i * 37 + 11) & 0xFF);
		}
		const bool bOpened = Inspector->OpenScratchArchiveFromBytes(Garbage, TEXT("not-an-archive.txt"));
		TestFalse(TEXT("non-archive file: Open reports failure"), bOpened);
		TestTrue(TEXT("non-archive file: status line carries a bad-format rejection"),
			Inspector->GetArchiveOpenStatus().Contains(TEXT("bad format")));
		TestEqual(TEXT("non-archive file: primary source unchanged (still the asset)"),
			static_cast<uint8>(Inspector->GetPrimarySource().Kind), static_cast<uint8>(FSuperFAISSInspectionSource::EKind::Asset));
	}

	// The positive control: a well-formed archive succeeds, supersedes the asset
	// selection, and the resolved primary source reflects it (dims/count/metric).
	{
		const bool bOpened = Inspector->OpenScratchArchiveFromBytes(ValidBytes, TEXT("valid.bin"));
		TestTrue(TEXT("valid archive: Open succeeds"), bOpened);
		TestTrue(TEXT("valid archive: no rejection status left behind"),
			Inspector->GetArchiveOpenStatus().IsEmpty());
		const FSuperFAISSInspectionSource Source = Inspector->GetPrimarySource();
		TestEqual(TEXT("valid archive: primary source kind is Archive"),
			static_cast<uint8>(Source.Kind), static_cast<uint8>(FSuperFAISSInspectionSource::EKind::Archive));
		TestEqual(TEXT("valid archive: source count matches the fixture"), Source.GetCount(), 20);
		TestEqual(TEXT("valid archive: source dims matches the fixture"), Source.GetDims(), 6);
		TestTrue(TEXT("valid archive: source metric matches the fixture"),
			Source.GetMetric() == ESuperFAISSBankMetric::L2);
	}
	return true;
}

// The second-bank slot's mirror (temper W1): the SAME open/reject affordance and
// mutual-exclusion rule on the second slot, exercised once (not the full three-way
// rejection matrix again -- that machinery is shared, proven above; this cell's own
// incremental claim is that the SECOND slot's own wiring, not the shared rejection
// path, is real too).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorSecondArchiveOpenAndMutualExclusionTest,
	"SuperFAISS.D.InspectorSecondArchiveOpenAndMutualExclusion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorSecondArchiveOpenAndMutualExclusionTest::RunTest(const FString& Parameters)
{
	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);

	USuperFAISSVectorBank* SecondAsset = MakeBank(*this, SeededRows(10, 4, 0xB001), 10, 4,
		ESuperFAISSBankMetric::L2, ESuperFAISSBankQuantization::Float32);
	if (SecondAsset == nullptr)
	{
		return true;
	}
	Inspector->SetSecondBankForTest(SecondAsset);
	TestEqual(TEXT("(setup) second source is the asset"),
		static_cast<uint8>(Inspector->GetSecondSource().Kind), static_cast<uint8>(FSuperFAISSInspectionSource::EKind::Asset));

	TArray<uint8> Bytes;
	if (!MakeScratchArchiveBytes(*this, SeededRows(15, 4, 0xB002), 15, 4, ESuperFAISSBankMetric::L2,
		ESuperFAISSBankQuantization::Float32, {}, Bytes))
	{
		return true;
	}
	TestTrue(TEXT("second archive open: succeeds"),
		Inspector->OpenSecondScratchArchiveFromBytes(Bytes, TEXT("second.bin")));
	TestEqual(TEXT("second archive open: second source is now Archive, superseding the asset"),
		static_cast<uint8>(Inspector->GetSecondSource().Kind), static_cast<uint8>(FSuperFAISSInspectionSource::EKind::Archive));

	// Reversing direction: picking an asset supersedes the open archive.
	Inspector->SetSecondBankForTest(SecondAsset);
	TestEqual(TEXT("second asset re-select: supersedes the open archive"),
		static_cast<uint8>(Inspector->GetSecondSource().Kind), static_cast<uint8>(FSuperFAISSInspectionSource::EKind::Asset));
	return true;
}

// Dim across structural claims (the documented asymmetry, section 25.3's own callout):
// GetTombstoneWords() is ALWAYS empty for an Asset-kind source and a real, correctly-
// sized (possibly all-zero) buffer for an Archive-kind source; GetIdForIndex/
// GetIndexForId always NAME_None/INDEX_NONE on an Archive source (ScratchBank carries no
// id table at all). GREEN AT AUTHORING TIME -- FSuperFAISSInspectionSource's own methods
// are real, mechanical dispatch (section 1 of the test-design artifact); kept as a
// standing regression guard against the asymmetry being silently "fixed" into a false
// symmetry later.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorInspectionSourceAsymmetryTest,
	"SuperFAISS.D.InspectorInspectionSourceAsymmetry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorInspectionSourceAsymmetryTest::RunTest(const FString& Parameters)
{
	// MakeBank's shared helper doesn't take Ids -- InitFromSource is called directly for
	// an id-bearing bank instead, since this cell needs a REAL id to contrast against the
	// archive's always-NAME_None.
	USuperFAISSVectorBank* IdBank = NewObject<USuperFAISSVectorBank>();
	FString Error;
	const TArray<FName> Ids = {TEXT("alpha"), TEXT("beta"), TEXT("gamma")};
	const bool bBuilt = IdBank->InitFromSource(SeededRows(3, 4, 0xC002), 3, 4,
		ESuperFAISSBankMetric::L2, ESuperFAISSBankQuantization::Float32, Ids, TEXT("asym-test"), Error);
	TestTrue(FString::Printf(TEXT("id-bank built: %s"), *Error), bBuilt);
	if (!bBuilt)
	{
		return true;
	}

	TArray<uint8> Bytes;
	if (!MakeScratchArchiveBytes(*this, SeededRows(3, 4, 0xC003), 3, 4, ESuperFAISSBankMetric::L2,
		ESuperFAISSBankQuantization::Float32, {}, Bytes))
	{
		return true;
	}

	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	Inspector->SetBankForTest(IdBank);
	const FSuperFAISSInspectionSource AssetSource = Inspector->GetPrimarySource();
	TestEqual(TEXT("asset source: GetTombstoneWords() is empty"), AssetSource.GetTombstoneWords().Num(), 0);
	TestEqual(TEXT("asset source: GetIdForIndex resolves a real id"), AssetSource.GetIdForIndex(1), FName(TEXT("beta")));
	TestEqual(TEXT("asset source: GetIndexForId resolves a real index"), AssetSource.GetIndexForId(TEXT("gamma")), 2);

	Inspector->OpenScratchArchiveFromBytes(Bytes, TEXT("asym.bin"));
	const FSuperFAISSInspectionSource ArchiveSource = Inspector->GetPrimarySource();
	TestEqual(TEXT("archive source: GetTombstoneWords() is real and correctly sized (all-zero, nothing removed)"),
		ArchiveSource.GetTombstoneWords().Num(), superfaiss::ScratchBank::TombstoneWords(3));
	for (const uint32 Word : ArchiveSource.GetTombstoneWords())
	{
		TestEqual(TEXT("archive source: no tombstones set (nothing removed)"), Word, 0u);
	}
	TestEqual(TEXT("archive source: GetIdForIndex is always NAME_None (no id table exists)"),
		ArchiveSource.GetIdForIndex(1), NAME_None);
	TestEqual(TEXT("archive source: GetIndexForId is always INDEX_NONE"),
		ArchiveSource.GetIndexForId(TEXT("beta")), static_cast<int32>(INDEX_NONE));
	return true;
}

// Dim 1 (audit F3, the NEW archive-swap leg): swapping archives without closing the
// widget must not leak archive #1's exclusion/tombstone state or live-row sample into
// archive #2's passes. Fully pass-level this round -- ComputeStructure() genuinely runs
// against an archive source (the real half of slot 4b's wiring) -- so this proves the
// SAME cache-clear rule (InvalidateAnalysisCaches) fires on an archive open exactly as
// it does on every other trigger, AND that a fresh compute over archive #2 reflects only
// archive #2's own rows.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorArchiveSwapInvalidationTest,
	"SuperFAISS.D.InspectorArchiveSwapInvalidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorArchiveSwapInvalidationTest::RunTest(const FString& Parameters)
{
	USuperFAISSVectorBank* Baseline = MakeBank(*this, SeededRows(20, 8, 0xD001), 20, 8,
		ESuperFAISSBankMetric::L2, ESuperFAISSBankQuantization::Float32);
	if (Baseline == nullptr)
	{
		return true;
	}

	TArray<uint8> Archive1Bytes, Archive2Bytes;
	if (!MakeScratchArchiveBytes(*this, SeededRows(30, 8, 0xD002), 30, 8, ESuperFAISSBankMetric::L2,
			ESuperFAISSBankQuantization::Float32, {}, Archive1Bytes) ||
		!MakeScratchArchiveBytes(*this, SeededRows(25, 8, 0xD003), 25, 8, ESuperFAISSBankMetric::L2,
			ESuperFAISSBankQuantization::Float32, {}, Archive2Bytes))
	{
		return true;
	}

	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);

	// Establish a real "previously computed" poison state via the asset path (proven
	// real since slot 3) before opening any archive.
	Inspector->SetBankForTest(Baseline);
	Inspector->ComputeStructure();
	TestTrue(TEXT("(setup) a Structure result exists before the first archive open"),
		Inspector->GetStructureClusters().Num() > 0 || Inspector->GetStructureOutlierSampleIndices().Num() > 0);

	// Opening archive #1 clears the cache (the archive-swap leg fires on the FIRST open
	// too, not only a swap between two archives).
	TestTrue(TEXT("archive #1 opens"), Inspector->OpenScratchArchiveFromBytes(Archive1Bytes, TEXT("archive1.bin")));
	TestEqual(TEXT("opening archive #1 clears the prior Structure cache"),
		Inspector->GetStructureClusters().Num() + Inspector->GetStructureOutlierSampleIndices().Num(), 0);

	// A real compute over archive #1 (ComputeStructure is wired to GetPrimarySource()
	// this round -- ARCHIVE sources genuinely reach the pipeline now, even though the
	// space-law sampling gap remains, see the crux FEAT below).
	Inspector->ComputeStructure();
	TestFalse(TEXT("(setup) archive #1's Structure pass actually ran (a real status, not the rejection idiom)"),
		Inspector->GetStructureStatus() == TEXT("no valid bank selected"));

	// Swapping to archive #2 without closing the widget clears the cache again -- no
	// leftover from archive #1 survives.
	TestTrue(TEXT("archive #2 opens"), Inspector->OpenScratchArchiveFromBytes(Archive2Bytes, TEXT("archive2.bin")));
	TestEqual(TEXT("swapping to archive #2 clears archive #1's Structure cache"),
		Inspector->GetStructureClusters().Num() + Inspector->GetStructureOutlierSampleIndices().Num(), 0);
	TestEqual(TEXT("primary source now resolves to archive #2's own count"),
		Inspector->GetPrimarySource().GetCount(), 25);
	return true;
}

// Dim 2/8/11b (SF34-005 -- converted from the pre-SF34-005
// InspectorArchiveChannelScopeRejection regression test, 2026-07-22: the outright
// channel-scope rejection this test used to pin is GONE by this ticket's own acceptance
// criterion, plan section 6 dim 5/11b -- "the former outright rejection is now a supported
// path... its old rejection cell is deleted, not left dangling." Converted rather than
// deleted outright so the fixture's real regression value survives: this is the unit-level
// companion to the tutorial-bank oracle's TutorialArchiveChannelScopeParity (which proves
// the golden VALUES); this test proves the STRUCTURAL contract -- a channel-scoped archive
// sample actually resolves to the channel's own dims/renormalized rows, not the whole row.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorArchiveChannelScopeSupportedTest,
	"SuperFAISS.D.InspectorArchiveChannelScopeSupported",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorArchiveChannelScopeSupportedTest::RunTest(const FString& Parameters)
{
	// A channel-carrying ASSET (to populate the shared scope combo -- the scope selector
	// is asset-driven regardless of which source is primary, section 25.3's design note)
	// alongside the archive under test.
	const TArray<FName> ChannelNames = {TEXT("chanA"), TEXT("chanB")};
	const TArray<int32> ChannelOffsets = {0, 8};
	const TArray<int32> ChannelLengths = {8, 8};
	USuperFAISSVectorBank* ChannelAsset = MakeBank(*this, SeededRows(10, 16, 0xE001), 10, 16,
		ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Float32,
		ChannelNames, ChannelOffsets, ChannelLengths);
	if (ChannelAsset == nullptr)
	{
		return true;
	}

	// The archive must carry the SAME channel table, not just channel-less rows --
	// Source.GetChannelIndex(chanA) resolves against the ARCHIVE's own ChannelNames
	// (FSuperFAISSInspectionSource::GetChannelIndex's Archive-kind branch), so a channel-
	// less archive (MakeScratchArchiveBytes's plain Init()) would legitimately reject
	// "unknown channel scope" here -- a different, correct rejection, not the one this test
	// is about. InitWithChannels directly, mirroring SuperFAISSInspectorTrustGapTests.cpp's
	// own channel-carrying archive fixture.
	USuperFAISSScratchBank* ChannelScratch = NewObject<USuperFAISSScratchBank>();
	if (!ChannelScratch->InitWithChannels(10, 16, ESuperFAISSBankMetric::Cosine,
		ESuperFAISSBankQuantization::Float32, ChannelNames, ChannelOffsets, ChannelLengths))
	{
		AddError(TEXT("(setup) channel-carrying archive InitWithChannels failed"));
		return true;
	}
	const TArray<float> ArchiveRows = SeededRows(10, 16, 0xE002);
	for (int32 i = 0; i < 10; ++i)
	{
		TArray<float> Row;
		Row.Append(&ArchiveRows[static_cast<int64>(i) * 16], 16);
		int32 OutIndex = INDEX_NONE;
		if (!ChannelScratch->Append(Row, OutIndex))
		{
			AddError(TEXT("(setup) channel-carrying archive row append failed"));
			return true;
		}
	}
	TArray<uint8> Bytes;
	if (!ChannelScratch->SaveToBytes(Bytes))
	{
		AddError(TEXT("(setup) channel-carrying archive SaveToBytes failed"));
		return true;
	}

	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	Inspector->SetBankForTest(ChannelAsset); // populates the scope combo
	Inspector->SetAnalysisScopeForTest(TEXT("chanA"));
	Inspector->OpenScratchArchiveFromBytes(Bytes, TEXT("channel-archive.bin"));

	TArray<uint8, TAlignedHeapAllocator<16>> Payload;
	TArray<float> Scales;
	superfaiss::BankView View;
	TArray<int32> SourceIndices;
	const bool bOk = Inspector->BuildAnalysisSampleForTest(Inspector->GetPrimarySource(), 8,
		Payload, Scales, View, SourceIndices);
	TestTrue(TEXT("SF34-005: channel-scoped archive analysis is a SUPPORTED path (the former "
		"outright rejection is genuinely replaced)"), bOk);
	if (bOk)
	{
		TestEqual(TEXT("SF34-005: the sampled view's dims equal the channel's own length (8), "
			"not the whole row's (16)"), View.dims, 8);
		TestEqual(TEXT("SF34-005: the sample count is min(live count, SampleLimit) = min(10, 8)"),
			View.count, 8);
	}

	// The whole-row scope remains real too -- confirms the fix is additive, not a scope-
	// specific regression on the path that already worked.
	Inspector->SetAnalysisScopeForTest(TEXT("(whole row)"));
	const bool bWholeRowOk = Inspector->BuildAnalysisSampleForTest(Inspector->GetPrimarySource(), 8,
		Payload, Scales, View, SourceIndices);
	TestTrue(TEXT("whole-row archive analysis is still real (unaffected by the channel-scope fix)"),
		bWholeRowOk);
	return true;
}

// Dim 4 (audit F4, archive count classes restated in LIVE-count terms): a fully-pruned
// archive (live 0) routes to the dim-2 rejection; a one-live-row archive (published >
// 1, only one row survives) admits exactly one sample; a published count at cap with
// live count far below it exercises the live-stride arithmetic (the sample must be
// drawn from the LIVE rows only, not the published range). All three legs are
// red-unimplemented: BuildAnalysisSample(Source, ...)'s scaffold has no live-count
// awareness at all (it strides over the PUBLISHED range unconditionally).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorArchiveLiveCountClassesTest,
	"SuperFAISS.D.InspectorArchiveLiveCountClasses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorArchiveLiveCountClassesTest::RunTest(const FString& Parameters)
{
	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	TArray<uint8, TAlignedHeapAllocator<16>> Payload;
	TArray<float> Scales;
	superfaiss::BankView View;
	TArray<int32> SourceIndices;

	// Leg 1: live 0 -- every published row tombstoned.
	{
		TArray<int32> RemoveAll = {0, 1, 2, 3, 4};
		TArray<uint8> Bytes;
		if (MakeScratchArchiveBytes(*this, SeededRows(5, 4, 0xF001), 5, 4, ESuperFAISSBankMetric::L2,
			ESuperFAISSBankQuantization::Float32, RemoveAll, Bytes))
		{
			Inspector->OpenScratchArchiveFromBytes(Bytes, TEXT("live0.bin"));
			const bool bOk = Inspector->BuildAnalysisSampleForTest(Inspector->GetPrimarySource(), 8,
				Payload, Scales, View, SourceIndices);
			TestFalse(TEXT("live-0 archive: sample construction rejects (nothing live to sample)"), bOk);
		}
	}

	// Leg 2: one live row survives out of a larger published count.
	{
		TArray<int32> RemoveAllButOne = {1, 2, 3, 4, 5, 6, 7};
		TArray<uint8> Bytes;
		if (MakeScratchArchiveBytes(*this, SeededRows(8, 4, 0xF002), 8, 4, ESuperFAISSBankMetric::L2,
			ESuperFAISSBankQuantization::Float32, RemoveAllButOne, Bytes))
		{
			Inspector->OpenScratchArchiveFromBytes(Bytes, TEXT("live1.bin"));
			const bool bOk = Inspector->BuildAnalysisSampleForTest(Inspector->GetPrimarySource(), 8,
				Payload, Scales, View, SourceIndices);
			if (bOk)
			{
				TestEqual(TEXT("live-1 archive: the sample admits exactly the one live row"), View.count, 1);
				TestEqual(TEXT("live-1 archive: the sampled source index is the surviving row (0)"),
					SourceIndices.Num() > 0 ? SourceIndices[0] : -1, 0);
			}
			else
			{
				AddError(TEXT("live-1 archive: sample construction rejected a genuinely live-1 archive"));
			}
		}
	}

	// Leg 3: published count at (a small) cap with live count far below it -- the
	// live-stride arithmetic must draw its sample from the live rows, never touching a
	// tombstoned one. 40 published, 10 live (30 tombstoned, interleaved so a naive
	// published-range stride cannot dodge them by luck).
	{
		TArray<int32> Tombstoned;
		for (int32 i = 0; i < 40; ++i)
		{
			if (i % 4 != 0) { Tombstoned.Add(i); } // keeps 0,4,8,...,36 live (10 rows)
		}
		TArray<uint8> Bytes;
		if (MakeScratchArchiveBytes(*this, SeededRows(40, 4, 0xF003), 40, 4, ESuperFAISSBankMetric::L2,
			ESuperFAISSBankQuantization::Float32, Tombstoned, Bytes))
		{
			Inspector->OpenScratchArchiveFromBytes(Bytes, TEXT("livestride.bin"));
			const FSuperFAISSInspectionSource Source = Inspector->GetPrimarySource();
			TestEqual(TEXT("(setup) published count is 40"), Source.GetCount(), 40);
			TestEqual(TEXT("(setup) live count is 10"), Source.GetLiveCount(), 10);
			const bool bOk = Inspector->BuildAnalysisSampleForTest(Source, 8, Payload, Scales, View, SourceIndices);
			if (bOk)
			{
				bool bAnyTombstonedSampled = false;
				for (const int32 Idx : SourceIndices)
				{
					if (Idx % 4 != 0) { bAnyTombstonedSampled = true; }
				}
				TestFalse(TEXT("live-stride: no sample position resolves to a tombstoned source row"),
					bAnyTombstonedSampled);
			}
		}
	}
	return true;
}

// Dim 8 / the space law (audit F2 -- the space-law cell): a constructed case proving the
// space law is neither inert nor redundant. A tombstoned row at the LAST published index
// is guaranteed sampled by the endpoint-inclusive construction (it always includes
// source index Full.count-1) UNLESS the live-only skip runs first -- so "the last sample
// position never resolves to a tombstoned source index" is a deterministic, always-
// meaningful proof of the skip, independent of stride-rounding arithmetic.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorArchiveSpaceLawSampleTest,
	"SuperFAISS.D.InspectorArchiveSpaceLawSample",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorArchiveSpaceLawSampleTest::RunTest(const FString& Parameters)
{
	// 3000 published rows (above the default SampleLimit=2048, so striding genuinely
	// engages), the LAST 100 tombstoned (2900..2999).
	TArray<int32> Tombstoned;
	for (int32 i = 2900; i < 3000; ++i) { Tombstoned.Add(i); }
	TArray<uint8> Bytes;
	if (!MakeScratchArchiveBytes(*this, SeededRows(3000, 4, 0xA1A1), 3000, 4, ESuperFAISSBankMetric::L2,
		ESuperFAISSBankQuantization::Float32, Tombstoned, Bytes))
	{
		return true;
	}

	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	Inspector->OpenScratchArchiveFromBytes(Bytes, TEXT("spacelaw.bin"));

	TArray<uint8, TAlignedHeapAllocator<16>> Payload;
	TArray<float> Scales;
	superfaiss::BankView View;
	TArray<int32> SourceIndices;
	const bool bOk = Inspector->BuildAnalysisSampleForTest(Inspector->GetPrimarySource(), 2048,
		Payload, Scales, View, SourceIndices);
	TestTrue(TEXT("(setup) sample construction succeeded"), bOk);
	if (bOk)
	{
		TestTrue(TEXT("space law: the last sample position does not resolve to a tombstoned source row"),
			SourceIndices.Num() == 0 || SourceIndices.Last() < 2900);
		bool bAnyTombstonedSampled = false;
		for (const int32 Idx : SourceIndices)
		{
			if (Idx >= 2900) { bAnyTombstonedSampled = true; }
		}
		TestFalse(TEXT("space law: no sample position resolves to any tombstoned source row"),
			bAnyTombstonedSampled);
	}
	return true;
}

// Dim 10 (audit F1, the transparency/equality half -- distinct from the crux FEAT below):
// a zero-tombstone archive (nothing ever removed) produces Structure output BIT-IDENTICAL
// to the same rows baked as an asset. GREEN AT AUTHORING TIME -- with nothing tombstoned,
// BuildAnalysisSample(Source, ...)'s scaffold strides over the published range exactly as
// the asset path does, so archive and asset agree by construction ALREADY. This is
// EXACTLY why audit F1 says equality alone cannot prove the space law: this cell passes
// today, before the space law is built, precisely because it never exercises a
// tombstone -- the crux FEAT below (a PRUNED archive, tombstones present) is the
// SEPARATE, CONSTRUCTED-TRUTH cell that actually catches the gap this one cannot see.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorStructureArchiveTransparencyEqualityTest,
	"SuperFAISS.D.InspectorStructureArchiveTransparencyEquality",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorStructureArchiveTransparencyEqualityTest::RunTest(const FString& Parameters)
{
	const TArray<float> Rows = SeededRows(60, 8, 0xB2B2);
	USuperFAISSVectorBank* BakedAsset = MakeBank(*this, Rows, 60, 8,
		ESuperFAISSBankMetric::L2, ESuperFAISSBankQuantization::Float32);
	TArray<uint8> ArchiveBytes;
	if (BakedAsset == nullptr ||
		!MakeScratchArchiveBytes(*this, Rows, 60, 8, ESuperFAISSBankMetric::L2,
			ESuperFAISSBankQuantization::Float32, {}, ArchiveBytes))
	{
		return true;
	}

	TSharedRef<SSuperFAISSBankInspector> AssetInspector = SNew(SSuperFAISSBankInspector);
	AssetInspector->SetBankForTest(BakedAsset);
	AssetInspector->ComputeStructure();

	TSharedRef<SSuperFAISSBankInspector> ArchiveInspector = SNew(SSuperFAISSBankInspector);
	ArchiveInspector->OpenScratchArchiveFromBytes(ArchiveBytes, TEXT("transparency.bin"));
	ArchiveInspector->ComputeStructure();

	TestEqual(TEXT("transparency: same component count, archive vs baked asset"),
		ArchiveInspector->GetStructureClusters().Num(), AssetInspector->GetStructureClusters().Num());
	TestEqual(TEXT("transparency: same outlier count, archive vs baked asset"),
		ArchiveInspector->GetStructureOutlierSampleIndices().Num(),
		AssetInspector->GetStructureOutlierSampleIndices().Num());
	TestTrue(TEXT("transparency: same component-id-by-sample-index array, archive vs baked asset"),
		ArchiveInspector->GetStructureComponentIdBySampleIndex() ==
		AssetInspector->GetStructureComponentIdBySampleIndex());
	return true;
}

// Launch-gate finding S2: ComputeStructure's status line must report the sample fraction
// against the LIVE (sampleable) count, not the published count -- BuildAnalysisSample is
// called with bSkipTombstonedRows == true, so a tombstoned row can never be sampled and a
// denominator of the published count sits above what the sample could ever reach on a
// pruned archive. 60 published, 24 tombstoned (interleaved so a naive stride cannot dodge
// them by luck), 36 live -- above the default StructureK (16) so ComputeStructure runs a
// real pass, not the "sample too small" rejection.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorStructureStatusLiveCountTest,
	"SuperFAISS.D.InspectorStructureStatusLiveCount",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorStructureStatusLiveCountTest::RunTest(const FString& Parameters)
{
	TArray<int32> Tombstoned;
	for (int32 i = 0; i < 60; ++i)
	{
		if (i % 5 < 2) { Tombstoned.Add(i); } // removes 2 of every 5 -> 24 removed, 36 live
	}
	TArray<uint8> Bytes;
	if (!MakeScratchArchiveBytes(*this, SeededRows(60, 8, 0x52C0), 60, 8, ESuperFAISSBankMetric::L2,
			ESuperFAISSBankQuantization::Float32, Tombstoned, Bytes))
	{
		return true;
	}

	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	Inspector->OpenScratchArchiveFromBytes(Bytes, TEXT("livecount-status.bin"));
	const FSuperFAISSInspectionSource Source = Inspector->GetPrimarySource();
	TestEqual(TEXT("(setup) published count is 60"), Source.GetCount(), 60);
	TestEqual(TEXT("(setup) live count is 36"), Source.GetLiveCount(), 36);

	Inspector->ComputeStructure();
	const FString& Status = Inspector->GetStructureStatus();
	TestTrue(TEXT("structure status: denominator is the live count (36), not the published count (60)"),
		Status.Contains(TEXT("of 36 rows")));
	TestFalse(TEXT("structure status: never reports the unreachable published count as the denominator"),
		Status.Contains(TEXT("of 60 rows")));
	return true;
}

// Dim 10 (the crux): the PRUNED-archive Structure FEAT. A bank of 3
// well-separated blobs (planted, constructed truth), GROWN with a 4th, equally
// well-separated blob, then that 4th blob Remove'd (tombstoned) and serialized -- the
// archive-sourced Structure pass must report EXACTLY the 3 live blobs as components,
// with NO sampled row resolving to the tombstoned 4th blob's source range. Sized above
// the widget's default SampleLimit (2048) so live-only striding genuinely engages (the
// fixture-engagement law). Constructed truth, not equality (audit F1) -- this is the
// fixture that actually catches the gap the transparency cell above cannot see.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorStructurePrunedArchivePanelFeatTest,
	"SuperFAISS.D.InspectorStructurePrunedArchivePanelFeat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorStructurePrunedArchivePanelFeatTest::RunTest(const FString& Parameters)
{
	FRandomStream Rng(0xC3C3);
	const int32 Dims = 16;
	const int32 LiveBlobSize = 700; // 3 live blobs = 2100 rows -- above SampleLimit=2048
	const int32 GrownBlobSize = 60; // planted-then-removed 4th blob

	TArray<float> Rows;
	for (int32 Bl = 0; Bl < 3; ++Bl)
	{
		TArray<float> Centre;
		Centre.SetNumZeroed(Dims);
		Centre[Bl * 4] = 100.0f * (Bl + 1);
		PushBlob(Rows, Dims, Centre, LiveBlobSize, Rng, 0.05f);
	}
	const int32 LiveCount = 3 * LiveBlobSize; // 2100
	TArray<float> GrownCentre;
	GrownCentre.SetNumZeroed(Dims);
	GrownCentre[Dims - 1] = 900.0f; // well separated from every live blob's axis
	PushBlob(Rows, Dims, GrownCentre, GrownBlobSize, Rng, 0.05f);
	const int32 TotalCount = LiveCount + GrownBlobSize; // 2160
	TestTrue(TEXT("fixture exceeds the widget's default SampleLimit (2048)"), TotalCount > 2048);

	TArray<int32> GrownIndices;
	for (int32 i = LiveCount; i < TotalCount; ++i) { GrownIndices.Add(i); }

	TArray<uint8> ArchiveBytes;
	if (!MakeScratchArchiveBytes(*this, Rows, TotalCount, Dims, ESuperFAISSBankMetric::L2,
		ESuperFAISSBankQuantization::Float32, GrownIndices, ArchiveBytes))
	{
		return true;
	}

	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	Inspector->OpenScratchArchiveFromBytes(ArchiveBytes, TEXT("pruned-structure.bin"));
	const FSuperFAISSInspectionSource Source = Inspector->GetPrimarySource();
	TestEqual(TEXT("(setup) published count includes the grown blob"), Source.GetCount(), TotalCount);
	TestEqual(TEXT("(setup) live count excludes the grown blob"), Source.GetLiveCount(), LiveCount);

	Inspector->ComputeStructure();

	// The absolute claim (dim 7, "a deleted row can never score as live"): no cluster
	// member or outlier sample resolves to a source row in the tombstoned range.
	const TArray<int32>& SampleSourceIndices = Inspector->GetStructureSampleSourceIndices();
	bool bAnyTombstonedRowInOutput = false;
	for (const int32 SourceIdx : SampleSourceIndices)
	{
		if (SourceIdx >= LiveCount) { bAnyTombstonedRowInOutput = true; }
	}
	TestFalse(TEXT("crux: no sampled row resolves to the tombstoned (grown-then-removed) blob"),
		bAnyTombstonedRowInOutput);

	// The constructed truth (not mere equality, audit F1): exactly 3 components at
	// MinComponentSize (the default, 3) or above -- the tombstoned 4th blob must never
	// surface as its own component.
	int32 BigComponents = 0;
	for (const FSuperFAISSStructureCluster& Cluster : Inspector->GetStructureClusters())
	{
		if (Cluster.MemberSampleIndices.Num() >= 3) { ++BigComponents; }
	}
	TestEqual(TEXT("crux: exactly the 3 LIVE blobs are reported as components"), BigComponents, 3);
	return true;
}

// Dim 10 (the crux -- Correspondence's own slot-A leg): the PRUNED-
// archive Correspondence FEAT with the archive in SLOT A (primary), a baked asset in
// slot B. Uses the CorrespondencePanelFeatTest's own grouped one-hot geometry (ported,
// not re-derived -- section 25.9's separation arithmetic already proven there), GROWN
// with extra A-only landmarks that are then Remove'd (tombstoned) and serialized. The
// absolute claim: no reported SourceIndexA is a tombstoned index (a deleted row can
// never surface as "checked" at all, let alone matched).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorCorrespondencePrunedArchiveSlotATest,
	"SuperFAISS.D.InspectorCorrespondencePrunedArchiveSlotA",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorCorrespondencePrunedArchiveSlotATest::RunTest(const FString& Parameters)
{
	const int32 Dims = 8;
	const int32 Groups = 8;
	const int32 SlotsPerGroup = 300;
	const int32 LandmarkCount = Groups * SlotsPerGroup; // 2400 -- exceeds SampleLimit=2048
	const int32 GrownCount = 40; // A-only, planted-then-removed

	auto MakeLandmarkRow = [Dims](int32 Group, float Magnitude) -> TArray<float>
	{
		TArray<float> Row;
		Row.SetNumZeroed(Dims);
		Row[Group] = Magnitude;
		return Row;
	};

	TArray<float> ARows;
	for (int32 i = 0; i < LandmarkCount; ++i)
	{
		const int32 Group = i / SlotsPerGroup;
		const int32 Slot = i % SlotsPerGroup;
		ARows.Append(MakeLandmarkRow(Group, 1000.0f + 10.0f * static_cast<float>(Slot)));
	}
	// A-only grown-then-removed landmarks, well past every real landmark's magnitude
	// range on their own axis (axis 7's real range tops out at 1000+10*299=3990).
	for (int32 g = 0; g < GrownCount; ++g)
	{
		ARows.Append(MakeLandmarkRow(7, 5000.0f + 10.0f * static_cast<float>(g)));
	}
	const int32 ACount = LandmarkCount + GrownCount;
	TArray<int32> GrownIndices;
	for (int32 i = LandmarkCount; i < ACount; ++i) { GrownIndices.Add(i); }

	TArray<float> BRows;
	for (int32 i = 0; i < LandmarkCount; ++i)
	{
		const int32 Landmark = LandmarkCount - 1 - i; // reversed permutation
		const int32 Group = Landmark / SlotsPerGroup;
		const int32 Slot = Landmark % SlotsPerGroup;
		BRows.Append(MakeLandmarkRow(Group, 1000.0f + 10.0f * static_cast<float>(Slot)));
	}
	const int32 BCount = LandmarkCount;

	TArray<uint8> ArchiveBytesA;
	if (!MakeScratchArchiveBytes(*this, ARows, ACount, Dims, ESuperFAISSBankMetric::L2,
		ESuperFAISSBankQuantization::Float32, GrownIndices, ArchiveBytesA))
	{
		return true;
	}
	USuperFAISSVectorBank* B = MakeBank(*this, BRows, BCount, Dims,
		ESuperFAISSBankMetric::L2, ESuperFAISSBankQuantization::Float32);
	if (B == nullptr)
	{
		return true;
	}

	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	Inspector->OpenScratchArchiveFromBytes(ArchiveBytesA, TEXT("pruned-correspondence-a.bin"));
	Inspector->SetSecondBankForTest(B);
	const FSuperFAISSInspectionSource PrimarySource = Inspector->GetPrimarySource();
	TestEqual(TEXT("(setup) slot A published count includes the grown landmarks"), PrimarySource.GetCount(), ACount);
	TestEqual(TEXT("(setup) slot A live count excludes them"), PrimarySource.GetLiveCount(), LandmarkCount);

	Inspector->ComputeCorrespondence();

	const TArray<FSuperFAISSMatchPairResult>& Pairs = Inspector->GetMatchPairResults();
	bool bAnyGrownRowChecked = false;
	for (const FSuperFAISSMatchPairResult& Pair : Pairs)
	{
		if (Pair.SourceIndexA >= LandmarkCount) { bAnyGrownRowChecked = true; }
	}
	TestFalse(TEXT("crux (slot A): no reported SourceIndexA is a tombstoned (grown-then-removed) row"),
		bAnyGrownRowChecked);
	return true;
}

// Dim 10 (temper W1's own explicit ask -- "exercised in slot A and in slot B
// separately"): the SAME PRUNED-archive Correspondence geometry with the archive in
// SLOT B (second), a baked asset in slot A. GREEN AT AUTHORING TIME -- an HONEST,
// asymmetric finding worth stating plainly (not the mirror-image red cell slot A gets):
// B is NEVER the A-side sample role inside ComputeCorrespondence() (only the primary
// plays that role, and only that role hits BuildAnalysisSample(Source, ...)'s still-
// missing live-only skip) -- B only ever appears as a FULL view, and the full-view
// runtime-OR leg (ExcludeBitsFullB, wired real this round) already, correctly, excludes
// every tombstoned B row from ever being retrieved as a match candidate. So archive-as-
// slot-B correspondence is ALREADY functionally correct for "a deleted row can never
// surface as a reported partner" -- a genuinely working capability, not a gap -- while
// archive-as-slot-A (the previous cell) is not. Kept as a standing regression guard
// against that specific asymmetry being lost.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorCorrespondencePrunedArchiveSlotBTest,
	"SuperFAISS.D.InspectorCorrespondencePrunedArchiveSlotB",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorCorrespondencePrunedArchiveSlotBTest::RunTest(const FString& Parameters)
{
	const int32 Dims = 8;
	const int32 Groups = 8;
	const int32 SlotsPerGroup = 300;
	const int32 LandmarkCount = Groups * SlotsPerGroup; // 2400
	const int32 GrownCount = 40; // B-only, planted-then-removed

	auto MakeLandmarkRow = [Dims](int32 Group, float Magnitude) -> TArray<float>
	{
		TArray<float> Row;
		Row.SetNumZeroed(Dims);
		Row[Group] = Magnitude;
		return Row;
	};

	TArray<float> ARows;
	for (int32 i = 0; i < LandmarkCount; ++i)
	{
		const int32 Group = i / SlotsPerGroup;
		const int32 Slot = i % SlotsPerGroup;
		ARows.Append(MakeLandmarkRow(Group, 1000.0f + 10.0f * static_cast<float>(Slot)));
	}
	const int32 ACount = LandmarkCount;

	TArray<float> BRows;
	for (int32 i = 0; i < LandmarkCount; ++i)
	{
		const int32 Landmark = LandmarkCount - 1 - i; // reversed permutation
		const int32 Group = Landmark / SlotsPerGroup;
		const int32 Slot = Landmark % SlotsPerGroup;
		BRows.Append(MakeLandmarkRow(Group, 1000.0f + 10.0f * static_cast<float>(Slot)));
	}
	// B-only grown-then-removed landmarks, an even BETTER (closer) decoy for A's axis-7
	// landmarks than their true reversed partner -- if these leaked into the full-B view
	// unexcluded they would steal the match; the space law's runtime-OR leg (wired real
	// this round) is what is supposed to prevent that, tested separately below. This
	// cell's OWN claim is narrower: they never appear as a REPORTED SourceIndexB at all.
	for (int32 g = 0; g < GrownCount; ++g)
	{
		BRows.Append(MakeLandmarkRow(0, 985.0f + 0.1f * static_cast<float>(g)));
	}
	const int32 BCount = LandmarkCount + GrownCount;
	TArray<int32> GrownIndices;
	for (int32 i = LandmarkCount; i < BCount; ++i) { GrownIndices.Add(i); }

	USuperFAISSVectorBank* A = MakeBank(*this, ARows, ACount, Dims,
		ESuperFAISSBankMetric::L2, ESuperFAISSBankQuantization::Float32);
	TArray<uint8> ArchiveBytesB;
	if (A == nullptr ||
		!MakeScratchArchiveBytes(*this, BRows, BCount, Dims, ESuperFAISSBankMetric::L2,
			ESuperFAISSBankQuantization::Float32, GrownIndices, ArchiveBytesB))
	{
		return true;
	}

	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	Inspector->SetBankForTest(A);
	Inspector->OpenSecondScratchArchiveFromBytes(ArchiveBytesB, TEXT("pruned-correspondence-b.bin"));
	const FSuperFAISSInspectionSource SecondSource = Inspector->GetSecondSource();
	TestEqual(TEXT("(setup) slot B published count includes the grown landmarks"), SecondSource.GetCount(), BCount);
	TestEqual(TEXT("(setup) slot B live count excludes them"), SecondSource.GetLiveCount(), LandmarkCount);

	Inspector->ComputeCorrespondence();

	const TArray<FSuperFAISSMatchPairResult>& Pairs = Inspector->GetMatchPairResults();
	bool bAnyGrownRowReported = false;
	for (const FSuperFAISSMatchPairResult& Pair : Pairs)
	{
		if (Pair.SourceIndexB >= LandmarkCount) { bAnyGrownRowReported = true; }
	}
	TestFalse(TEXT("crux (slot B): no reported SourceIndexB is a tombstoned (grown-then-removed) row"),
		bAnyGrownRowReported);
	return true;
}

// ComputeCorrespondence's status-line denominators used the PUBLISHED count
// (SecondSource.GetCount() / PrimarySource.GetCount()) instead of the LIVE count,
// overstating "unmatched (B)" and the "N of M A-rows checked" denominator by the
// tombstone total on an archive source. Both A and B are archives here, each carrying
// tombstoned decoy rows on well-separated one-hot landmark axes so every LIVE A row finds
// its exact mutual-NN partner in B (a perfect live-to-live matching): the correct
// unmatched-B count is 0 and the correct A-checked denominator equals the live A count.
// The published counts, inflated by the tombstones, are the wrong answer this cell fails
// on pre-fix.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorCorrespondenceLiveCountDenominatorsTest,
	"SuperFAISS.D.InspectorCorrespondenceLiveCountDenominators",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorCorrespondenceLiveCountDenominatorsTest::RunTest(const FString& Parameters)
{
	const int32 Dims = 16;
	const int32 LandmarkCount = 16; // one well-separated one-hot axis per landmark -- must
		// exceed USuperFAISSInspectorSettings' default MatchK (10) so ClampedMatchK never
		// exceeds the live sample count

	auto MakeLandmarkRow = [Dims](int32 Axis, float Magnitude) -> TArray<float>
	{
		TArray<float> Row;
		Row.SetNumZeroed(Dims);
		Row[Axis] = Magnitude;
		return Row;
	};

	TArray<float> ARows;
	for (int32 i = 0; i < LandmarkCount; ++i)
	{
		ARows.Append(MakeLandmarkRow(i, 1000.0f));
	}
	// A-only tombstoned decoys: extra landmark-shaped rows on axis 0, removed after
	// Append -- inflate A's published count without adding a live row.
	const int32 TombstonesA = 3;
	for (int32 g = 0; g < TombstonesA; ++g)
	{
		ARows.Append(MakeLandmarkRow(0, 500.0f + static_cast<float>(g)));
	}
	const int32 ACount = LandmarkCount + TombstonesA;
	TArray<int32> TombstoneIndicesA;
	for (int32 i = LandmarkCount; i < ACount; ++i) { TombstoneIndicesA.Add(i); }

	TArray<float> BRows;
	for (int32 i = 0; i < LandmarkCount; ++i)
	{
		// Reversed permutation, same landmark axes/magnitudes as A, so every live A row
		// finds its exact mutual-NN partner in B.
		const int32 Landmark = LandmarkCount - 1 - i;
		BRows.Append(MakeLandmarkRow(Landmark, 1000.0f));
	}
	const int32 TombstonesB = 5;
	for (int32 g = 0; g < TombstonesB; ++g)
	{
		BRows.Append(MakeLandmarkRow(0, 700.0f + static_cast<float>(g)));
	}
	const int32 BCount = LandmarkCount + TombstonesB;
	TArray<int32> TombstoneIndicesB;
	for (int32 i = LandmarkCount; i < BCount; ++i) { TombstoneIndicesB.Add(i); }

	TArray<uint8> ArchiveBytesA, ArchiveBytesB;
	if (!MakeScratchArchiveBytes(*this, ARows, ACount, Dims, ESuperFAISSBankMetric::L2,
			ESuperFAISSBankQuantization::Float32, TombstoneIndicesA, ArchiveBytesA) ||
		!MakeScratchArchiveBytes(*this, BRows, BCount, Dims, ESuperFAISSBankMetric::L2,
			ESuperFAISSBankQuantization::Float32, TombstoneIndicesB, ArchiveBytesB))
	{
		return true;
	}

	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	Inspector->OpenScratchArchiveFromBytes(ArchiveBytesA, TEXT("live-denom-a.bin"));
	Inspector->OpenSecondScratchArchiveFromBytes(ArchiveBytesB, TEXT("live-denom-b.bin"));

	const FSuperFAISSInspectionSource PrimarySource = Inspector->GetPrimarySource();
	const FSuperFAISSInspectionSource SecondSource = Inspector->GetSecondSource();
	TestEqual(TEXT("(setup) A published count includes tombstones"), PrimarySource.GetCount(), ACount);
	TestEqual(TEXT("(setup) A live count excludes them"), PrimarySource.GetLiveCount(), LandmarkCount);
	TestEqual(TEXT("(setup) B published count includes tombstones"), SecondSource.GetCount(), BCount);
	TestEqual(TEXT("(setup) B live count excludes them"), SecondSource.GetLiveCount(), LandmarkCount);

	Inspector->ComputeCorrespondence();

	// Every live A row finds its exact live B partner: zero genuinely unmatched on either
	// side, and every live A row gets checked -- the correct denominators are both the
	// live count, never the tombstone-inflated published count.
	TestEqual(TEXT("A-checked denominator is the LIVE A count and both sides report 0 unmatched"),
		Inspector->GetCorrespondenceStatus(),
		FString::Printf(TEXT("%d of %d A-rows checked, 0 unmatched (A), 0 unmatched (B)"),
			LandmarkCount, LandmarkCount));
	return true;
}

// Finding 6 (regression on Finding 1, caught at pre-gate review): the REACHABLE half.
// A channel-scoped Cosine bank pair where B carries one row with zero energy in the
// scoped channel (not tombstoned -- an ordinary Asset source, no archive involved). That
// row is not IN the channel-scoped view at all and can never be matched, so it must never
// be counted as "unmatched (B)", and the "N of M A-rows checked" denominator must not
// include a row that was silently excluded from the sample. Both status-line numbers are
// currently overstated by exactly the zero-energy count -- the SAME published-vs-actual
// denominator class Finding 2 fixed, re-entered via zero-energy instead of tombstones.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorCorrespondenceZeroEnergyDenominatorsTest,
	"SuperFAISS.D.InspectorCorrespondenceZeroEnergyDenominators",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorCorrespondenceZeroEnergyDenominatorsTest::RunTest(const FString& Parameters)
{
	const TArray<FName> ChannelNames = {TEXT("chan0"), TEXT("chan1")};
	const TArray<int32> ChannelOffsets = {0, 16};
	const TArray<int32> ChannelLengths = {16, 16};
	const int32 Dims = 32;
	// One well-separated one-hot axis per landmark, within chan0 (length 16, room for up
	// to 16 axes). Must exceed USuperFAISSInspectorSettings' default MatchK (10) so
	// ClampedMatchK never exceeds the live sample count (the same fixture-sizing lesson
	// InspectorCorrespondenceLiveCountDenominators above already applies).
	const int32 LandmarkCount = 12;

	auto MakeLandmarkRow = [Dims](int32 Axis, float ChanZeroMagnitude) -> TArray<float>
	{
		TArray<float> Row;
		Row.SetNumZeroed(Dims);
		if (Axis >= 0)
		{
			Row[Axis] = ChanZeroMagnitude; // chan0 direction (Axis in [0, 16))
		}
		Row[16] = 5.0f; // chan1: constant nonzero, out of scope, keeps every whole row nonzero
		return Row;
	};

	TArray<float> ARows;
	for (int32 i = 0; i < LandmarkCount; ++i)
	{
		ARows.Append(MakeLandmarkRow(i, 10.0f));
	}

	// B: the SAME landmarks, same order (an exact 1:1 match set), with one EXTRA row (not
	// tombstoned) planted in the MIDDLE -- a mid-sample drop is the case that would
	// silently shift every later row's reported native index under compaction (Finding
	// 6b), not just miscount (Finding 6a).
	const int32 ZeroEnergyInsertAt = 6;
	TArray<float> BRows;
	for (int32 i = 0; i < ZeroEnergyInsertAt; ++i)
	{
		BRows.Append(MakeLandmarkRow(i, 10.0f));
	}
	BRows.Append(MakeLandmarkRow(-1, 0.0f)); // zero energy in chan0 (Axis < 0 sets nothing)
	for (int32 i = ZeroEnergyInsertAt; i < LandmarkCount; ++i)
	{
		BRows.Append(MakeLandmarkRow(i, 10.0f));
	}
	const int32 BCount = LandmarkCount + 1;

	USuperFAISSVectorBank* A = MakeBank(*this, ARows, LandmarkCount, Dims,
		ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Int8,
		ChannelNames, ChannelOffsets, ChannelLengths);
	USuperFAISSVectorBank* B = MakeBank(*this, BRows, BCount, Dims,
		ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Int8,
		ChannelNames, ChannelOffsets, ChannelLengths);
	if (A == nullptr || B == nullptr)
	{
		return true;
	}

	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	Inspector->SetBankForTest(A);
	Inspector->SetSecondBankForTest(B);
	Inspector->SetAnalysisScopeForTest(TEXT("chan0"));

	Inspector->ComputeCorrespondence();

	// Every A landmark finds its exact B partner; B's zero-energy row (native index
	// ZeroEnergyInsertAt) was never a candidate, so it is never "unmatched" -- 0 unmatched
	// on both sides, and all LandmarkCount A rows are checked against the LandmarkCount
	// truly matchable B rows, not B's published LandmarkCount+1.
	TestEqual(TEXT("status: correct denominators, 0 unmatched both sides, disclosed exclusion"),
		Inspector->GetCorrespondenceStatus(),
		FString::Printf(TEXT("%d of %d A-rows checked, 0 unmatched (A), 0 unmatched (B), "
			"1 excluded (zero energy in channel, B)"), LandmarkCount, LandmarkCount));

	// Index identity, the 6b symptom this fixture's mid-sample drop is shaped to catch:
	// the matched B partners are B's TRUE native indices -- landmark axis A's row sits at
	// native B index A (A < ZeroEnergyInsertAt) or A+1 (A >= ZeroEnergyInsertAt, shifted
	// past the inserted zero-energy row) -- never the compacted positions {0..LandmarkCount-1}
	// a mid-sample reindex would silently report.
	TSet<int32> MatchedB;
	for (const FSuperFAISSMatchPairResult& Pair : Inspector->GetMatchPairResults())
	{
		if (Pair.SourceIndexB >= 0)
		{
			MatchedB.Add(Pair.SourceIndexB);
		}
	}
	TestEqual(TEXT("matched B count"), MatchedB.Num(), LandmarkCount);
	TestFalse(TEXT("B's zero-energy row is never a matched partner"),
		MatchedB.Contains(ZeroEnergyInsertAt));
	bool bAllNative = true;
	for (int32 Axis = 0; Axis < LandmarkCount; ++Axis)
	{
		const int32 ExpectedNativeB = Axis < ZeroEnergyInsertAt ? Axis : Axis + 1;
		if (!MatchedB.Contains(ExpectedNativeB))
		{
			bAllNative = false;
		}
	}
	TestTrue(TEXT("matched B partners are native indices, not compacted positions"), bAllNative);

	return true;
}

// Dim 8, the POSITIVE proof that the space law's runtime-OR leg is real (distinct from
// the crux cells above, which prove the sample-side construction gap): a slot-B archive
// carries a decoy row that scores CLOSER to an A landmark than that landmark's true
// partner -- if unexcluded, the decoy would steal the match. The decoy is then
// Remove'd (tombstoned) before serializing. Because ComputeCorrespondence() ORs
// GetTombstoneWords() into excludeBitsB for the FULL-B view (real, wired this round,
// section 25.3's "the full-view evidence query carries the OR, in source space" applied
// to Correspondence's own scoring passes), the true partner must still be recovered.
// GREEN AT AUTHORING TIME -- this mechanism IS real, not scaffolded; kept as a standing
// regression guard AND as the audit-F2 "the OR is neither inert nor redundant" proof at
// the pass level.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorCorrespondenceFullViewTombstoneOrTest,
	"SuperFAISS.D.InspectorCorrespondenceFullViewTombstoneOr",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorCorrespondenceFullViewTombstoneOrTest::RunTest(const FString& Parameters)
{
	// 15 one-hot landmarks (axis i, magnitude 1000) so BOTH fullViewA (15 rows) and
	// fullViewB's non-excluded count (15) clear the trust boundary MutualNearestMatches
	// enforces against the settings CDO's default MatchK (10): "matchK greater than the
	// number of non-excluded rows in fullViewB OR in fullViewA" -> InvalidArgument. A
	// tiny 1-2 row fixture (this cell's first draft) executed and failed for exactly
	// this reason -- MatchK=10 rejected it outright before the exclusion logic under
	// test ever ran; corrected here, not a design change to the claim itself.
	const int32 Dims = 15;
	const int32 LandmarkCount = 15;
	auto MakeLandmarkRow = [Dims](int32 Axis) -> TArray<float>
	{
		TArray<float> Row;
		Row.SetNumZeroed(Dims);
		Row[Axis] = 1000.0f;
		return Row;
	};

	TArray<float> ARows;
	for (int32 i = 0; i < LandmarkCount; ++i) { ARows.Append(MakeLandmarkRow(i)); }

	// B: a DECOY at index 0 -- an EXACT duplicate of A[0]'s landmark (axis 0) -- followed
	// by the 15 true partners at indices [1, 16) (B[1+i] is A[i]'s own landmark). Ties
	// break to the LOWEST index (V32-G5), so the decoy at index 0 would win A[0]'s match
	// over its true partner at index 1 unless excluded. A[1..14]'s landmarks are unique
	// (only axis 0 is duplicated), so they are unaffected -- an isolating control.
	TArray<float> BRows;
	BRows.Append(MakeLandmarkRow(0)); // decoy, index 0
	for (int32 i = 0; i < LandmarkCount; ++i) { BRows.Append(MakeLandmarkRow(i)); } // true partners, indices 1..15
	const int32 BCount = LandmarkCount + 1; // 16
	const int32 DecoyIndex = 0;
	const int32 TruePartnerIndexForA0 = 1;

	USuperFAISSVectorBank* A = MakeBank(*this, ARows, LandmarkCount, Dims,
		ESuperFAISSBankMetric::L2, ESuperFAISSBankQuantization::Float32);
	TArray<uint8> ArchiveBytesB;
	if (A == nullptr ||
		!MakeScratchArchiveBytes(*this, BRows, BCount, Dims, ESuperFAISSBankMetric::L2,
			ESuperFAISSBankQuantization::Float32, {DecoyIndex}, ArchiveBytesB))
	{
		return true;
	}

	TSharedRef<SSuperFAISSBankInspector> Inspector = SNew(SSuperFAISSBankInspector);
	Inspector->SetBankForTest(A);
	Inspector->OpenSecondScratchArchiveFromBytes(ArchiveBytesB, TEXT("decoy.bin"));
	Inspector->ComputeCorrespondence();

	const TArray<FSuperFAISSMatchPairResult>& Pairs = Inspector->GetMatchPairResults();
	TestEqual(TEXT("(setup) all 15 A rows checked"), Pairs.Num(), LandmarkCount);
	if (Pairs.Num() == LandmarkCount)
	{
		TestEqual(TEXT("runtime OR (real, wired): A[0]'s tombstoned decoy never wins the tie"),
			Pairs[0].SourceIndexB, TruePartnerIndexForA0);
		bool bAnyDecoyReported = false;
		for (const FSuperFAISSMatchPairResult& Pair : Pairs)
		{
			if (Pair.SourceIndexB == DecoyIndex) { bAnyDecoyReported = true; }
		}
		TestFalse(TEXT("runtime OR (real, wired): the tombstoned decoy never appears as ANY reported partner"),
			bAnyDecoyReported);
	}
	return true;
}

// Dim 10 (audit F1, Correspondence's own transparency/equality half): a zero-tombstone
// archive in slot B (nothing ever removed) recovers the SAME pairing as the same rows
// baked as an asset. GREEN AT AUTHORING TIME for the SAME reason the Structure
// transparency cell above is -- equality alone does not exercise the tombstone path,
// which is exactly the crux cells' own job.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorCorrespondenceArchiveVsBakedTransparencyTest,
	"SuperFAISS.D.InspectorCorrespondenceArchiveVsBakedTransparency",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorCorrespondenceArchiveVsBakedTransparencyTest::RunTest(const FString& Parameters)
{
	USuperFAISSVectorBank* A = MakeBank(*this, SeededRows(15, 6, 0xD4D4), 15, 6,
		ESuperFAISSBankMetric::L2, ESuperFAISSBankQuantization::Float32);
	const TArray<float> BRows = SeededRows(15, 6, 0xD5D5);
	USuperFAISSVectorBank* BAsAsset = MakeBank(*this, BRows, 15, 6,
		ESuperFAISSBankMetric::L2, ESuperFAISSBankQuantization::Float32);
	TArray<uint8> BArchiveBytes;
	if (A == nullptr || BAsAsset == nullptr ||
		!MakeScratchArchiveBytes(*this, BRows, 15, 6, ESuperFAISSBankMetric::L2,
			ESuperFAISSBankQuantization::Float32, {}, BArchiveBytes))
	{
		return true;
	}

	TSharedRef<SSuperFAISSBankInspector> BakedInspector = SNew(SSuperFAISSBankInspector);
	BakedInspector->SetBankForTest(A);
	BakedInspector->SetSecondBankForTest(BAsAsset);
	BakedInspector->ComputeCorrespondence();

	TSharedRef<SSuperFAISSBankInspector> ArchiveInspector = SNew(SSuperFAISSBankInspector);
	ArchiveInspector->SetBankForTest(A);
	ArchiveInspector->OpenSecondScratchArchiveFromBytes(BArchiveBytes, TEXT("transparency-b.bin"));
	ArchiveInspector->ComputeCorrespondence();

	const TArray<FSuperFAISSMatchPairResult>& BakedPairs = BakedInspector->GetMatchPairResults();
	const TArray<FSuperFAISSMatchPairResult>& ArchivePairs = ArchiveInspector->GetMatchPairResults();
	TestEqual(TEXT("transparency: same checked-row count, archive-B vs baked-B"),
		ArchivePairs.Num(), BakedPairs.Num());
	bool bAllMatch = ArchivePairs.Num() == BakedPairs.Num();
	for (int32 i = 0; bAllMatch && i < ArchivePairs.Num(); ++i)
	{
		bAllMatch = ArchivePairs[i].SourceIndexA == BakedPairs[i].SourceIndexA &&
			ArchivePairs[i].SourceIndexB == BakedPairs[i].SourceIndexB &&
			ArchivePairs[i].State == BakedPairs[i].State;
	}
	TestTrue(TEXT("transparency: pair-for-pair identical, archive-B vs baked-B"), bAllMatch);
	return true;
}

// Dim 3 (applicable narrowly for M4, cited not re-derived): the existing
// InspectorConcurrencyGrepTargetTest (slot 3, already green) recursively scans EVERY
// .cpp/.h under Source/SuperFAISSUnrealEditor, which already covers every file this
// round touches (SSuperFAISSBankInspector.{h,cpp}, this test file) without any change to
// that test. Slot 4b introduces no async dispatch (OpenScratchArchiveFromBytes is
// synchronous by this round's own stated judgment call, section 1 of the test-design
// artifact) -- cited, not re-derived; no new cell authored here.

#endif // WITH_DEV_AUTOMATION_TESTS
