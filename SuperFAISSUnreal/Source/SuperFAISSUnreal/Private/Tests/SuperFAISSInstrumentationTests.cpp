// V3.2 plan section 25.10 slot 5 — the plugin plan section 5.1 "Instrumentation &
// Unreal Insights" bar, runtime-module half (the SuperFAISS trace channel, the CPU
// timing scopes around query submit / the per-chunk kernel / the top-k merge / the
// batch bank-pass / async dispatch / bank load / scratch-pool alloc-free, and
// STATGROUP_SuperFAISS's claim-backing counters). Coverage Model: section 25.9's M5
// dimension, covered by citation to plugin plan section 12's instrumentation block
// (B8, B9, and the "what is and isn't testable" split) rather than restated here —
// see the test-design artifact for the full re-cross.
//
// SCOPE NOTE (plugin plan section 12, "Instrumentation & Insights coverage" — read in
// full before touching this file): trace-scope EMISSION itself is explicitly NOT
// unit-testable ("asserting a macro fired only proves it was typed") — that half is a
// VISUAL Insights smoke gate owed to a human, not a cell in this suite.
// What IS testable and IS this file's scope: B8 (non-perturbation), B9 (counter
// fidelity for the counters section 12 names an oracle for), and the load-bearing
// read-only/non-perturbing code rule's mechanical half (a grep target, modeled on
// SuperFAISS.D.InspectorConcurrencyGrepTarget).

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SuperFAISSSubsystem.h"
#include "SuperFAISSVectorBank.h"
#include "Trace/Trace.h"

#include "superfaiss/superfaiss.h"

namespace
{
	TArray<float> MakeRows(int32 Count, int32 Dims, uint64 Seed)
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

	USuperFAISSVectorBank* MakeBank(FAutomationTestBase& Test, int32 Count, int32 Dims,
		ESuperFAISSBankMetric Metric, ESuperFAISSBankQuantization Quant, uint64 Seed)
	{
		USuperFAISSVectorBank* Bank = NewObject<USuperFAISSVectorBank>();
		FString Error;
		const bool bOk = Bank->InitFromSource(MakeRows(Count, Dims, Seed), Count, Dims, Metric,
			Quant, {}, TEXT("instrumentation-test"), Error);
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

	// FNV-1a 64 over a hit list, the same hasher shape SuperFAISS.B.CrossDeviceGoldenHash
	// uses — count, then each hit's index and score bits, rank order.
	struct FDeterminismHash
	{
		uint64 H = 1469598103934665603ull;

		void Bytes(const void* P, SIZE_T N)
		{
			const uint8* B = static_cast<const uint8*>(P);
			for (SIZE_T i = 0; i < N; ++i)
			{
				H = (H ^ B[i]) * 1099511628211ull;
			}
		}

		void Hits(const TArray<FSuperFAISSHit>& Hits)
		{
			const uint32 Count = static_cast<uint32>(Hits.Num());
			Bytes(&Count, 4);
			for (const FSuperFAISSHit& Hit : Hits)
			{
				const uint32 Index = static_cast<uint32>(Hit.Index);
				Bytes(&Index, 4);
				uint32 Bits;
				FMemory::Memcpy(&Bits, &Hit.Score, 4);
				Bytes(&Bits, 4);
			}
		}
	};

	// The B8 battery: B1 repeat-equality, B2 serial <-> parallel, and a cross-device
	// query (the "Exactness::CrossDevice golden hash where exercised" clause) — all run
	// through the SUBSYSTEM surface, since that is the only layer section 5.1's markup
	// touches (the core is untouched, so wrapping the core-level golden-hash battery in
	// a trace toggle would be a no-op by construction, not a meaningful B8 exercise).
	// Hashes every result into one running accumulator so ONE equality assertion covers
	// the whole battery, trace-OFF vs trace-ON.
	uint64 RunDeterminismBattery(USuperFAISSSubsystem& Subsystem, USuperFAISSVectorBank& Bank,
		IConsoleVariable& ParallelModeVar)
	{
		FDeterminismHash Hash;
		FSuperFAISSQueryArgs Args;
		Args.K = 12;

		constexpr int32 Variants = 5;

		// Serial.
		ParallelModeVar.Set(0);
		for (int32 V = 0; V < Variants; ++V)
		{
			TArray<FSuperFAISSHit> Hits;
			Subsystem.QuerySync(&Bank, MakeQuery(Bank.Dims, 4000 + V), Args, Hits);
			Hash.Hits(Hits);
		}

		// Parallel, including a repeat (B1) of the first variant.
		ParallelModeVar.Set(2);
		for (int32 V = 0; V < Variants; ++V)
		{
			TArray<FSuperFAISSHit> Hits;
			Subsystem.QuerySync(&Bank, MakeQuery(Bank.Dims, 4000 + V), Args, Hits);
			Hash.Hits(Hits);
		}
		{
			TArray<FSuperFAISSHit> Repeat;
			Subsystem.QuerySync(&Bank, MakeQuery(Bank.Dims, 4000), Args, Repeat);
			Hash.Hits(Repeat);
		}

		// Batch (the batch bank-pass span).
		{
			TArray<float> Queries;
			Queries.Reserve(3 * Bank.Dims);
			for (int32 M = 0; M < 3; ++M)
			{
				Queries.Append(MakeQuery(Bank.Dims, 4100 + M));
			}
			TArray<FSuperFAISSHit> BatchHits;
			TArray<int32> BatchCounts;
			Subsystem.QueryBatch(&Bank, Queries, 3, Args, BatchHits, BatchCounts);
			Hash.Hits(BatchHits);
		}

		// Cross-device, where exercised (int8 banks only — the caller guarantees this).
		{
			TArray<FSuperFAISSHit> XdHits;
			Subsystem.QuerySimilarCrossDevice(&Bank, MakeQuery(Bank.Dims, 4200), 10, XdHits);
			Hash.Hits(XdHits);
		}

		return Hash.H;
	}
}

// B8 (the load-bearing cell, plugin plan section 5.1 / section 12): the determinism
// gate runs with the SuperFAISS trace channel OFF and ON and must produce one
// identical result set / hash. Two distinct claims live in this one cell:
//   1. The channel EXISTS and toggles — the SuperFAISS channel is registered
//      (UE_TRACE_CHANNEL) and this call returns true.
//   2. Trace-OFF and trace-ON produce a bit-identical battery hash — the actual
//      non-perturbation proof: with real TRACE_CPUPROFILER_EVENT_SCOPE/stat-macro call
//      sites wired into the query path, this assertion is the load-bearing proof that
//      they move no bit. It is a standing regression guard, exactly the shape
//      SuperFAISS.D.InspectorNoveltySelfExclusionLowConfidence and
//      SuperFAISS.D.InspectorConcurrencyGrepTarget already use in this project.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInstrumentationNonPerturbationTest,
	"SuperFAISS.B.InstrumentationNonPerturbation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInstrumentationNonPerturbationTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	// 12000 x 96 int8: the same shape SuperFAISS.B.SerialParallelEquality uses to
	// guarantee a real (>1 chunk) parallel fan-out, so the battery actually exercises
	// the per-chunk/merge spans section 5.1 wraps, not just the serial fast path.
	USuperFAISSVectorBank* Bank = MakeBank(*this, 12000, 96, ESuperFAISSBankMetric::Cosine,
		ESuperFAISSBankQuantization::Int8, 0x8ED0ull);
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

	// Claim 1: the channel exists and toggles ON.
	const bool bToggledOn = UE::Trace::ToggleChannel(TEXT("SuperFAISS"), true);
	TestTrue(TEXT("the SuperFAISS trace channel exists and enables"), bToggledOn);

	// Claim 2, trace-OFF leg: force the channel off explicitly regardless of claim 1's
	// outcome, so the OFF leg is genuinely off even once the channel is real.
	UE::Trace::ToggleChannel(TEXT("SuperFAISS"), false);
	const uint64 HashOff = RunDeterminismBattery(*Subsystem, *Bank, *ModeVar);

	// Claim 2, trace-ON leg: same battery, same seeds, channel enabled.
	UE::Trace::ToggleChannel(TEXT("SuperFAISS"), true);
	const uint64 HashOn = RunDeterminismBattery(*Subsystem, *Bank, *ModeVar);

	// Restore the channel to its pre-test state (best-effort — a channel this suite
	// found already-registered by another system should not be left toggled).
	UE::Trace::ToggleChannel(TEXT("SuperFAISS"), false);
	ModeVar->Set(SavedMode);

	TestEqual(TEXT("determinism battery hash identical trace-OFF vs trace-ON"),
		HashOn, HashOff);
	return true;
}

// B9a: bytes-streamed fidelity. The oracle is superfaiss::BankBytes(view) — the exact
// core function query.cpp's own scan cost accounting is built from (count * paddedDims
// * ElementSize(quant)), i.e. the PHYSICAL traffic the SIMD kernel actually loads, not
// the section 5 model's logical N*Dims*B floor (plugin plan section 12's B9 note).
// Two quantizations so the ElementSize(quant) term is exercised on both its values (1
// for int8, 4 for float32) rather than pinned by construction to one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSCounterFidelityBytesStreamedTest,
	"SuperFAISS.B.CounterFidelityBytesStreamed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSCounterFidelityBytesStreamedTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	USuperFAISSVectorBank* Int8Bank = MakeBank(*this, 5000, 68, ESuperFAISSBankMetric::Cosine,
		ESuperFAISSBankQuantization::Int8, 0xB9E5ull);
	USuperFAISSVectorBank* F32Bank = MakeBank(*this, 3000, 68, ESuperFAISSBankMetric::Cosine,
		ESuperFAISSBankQuantization::Float32, 0xB9E6ull);
	if (!Int8Bank || !F32Bank)
	{
		return true;
	}

	FSuperFAISSQueryArgs Args;
	Args.K = 10;

	{
		TArray<FSuperFAISSHit> Hits;
		TestTrue(TEXT("int8 query ok"),
			Subsystem->QuerySync(Int8Bank, MakeQuery(68, 1), Args, Hits));
		const uint64 Expected = superfaiss::BankBytes(Int8Bank->GetBankView());
		TestEqual(TEXT("bytes streamed == N*PaddedDims*1 (int8)"),
			Subsystem->GetLastQueryBytesStreamed(), Expected);
	}
	{
		TArray<FSuperFAISSHit> Hits;
		TestTrue(TEXT("f32 query ok"),
			Subsystem->QuerySync(F32Bank, MakeQuery(68, 2), Args, Hits));
		const uint64 Expected = superfaiss::BankBytes(F32Bank->GetBankView());
		TestEqual(TEXT("bytes streamed == N*PaddedDims*4 (float32)"),
			Subsystem->GetLastQueryBytesStreamed(), Expected);
	}
	return true;
}

// B9b: worker/chunk-count fidelity. The oracle is "the actual scan partition" — 1 when
// the serial path ran, superfaiss::ChunkCount(view) when the parallel fan-out ran —
// not a single formula, because the counter must report what actually happened under
// whichever mode the CVar-selected path took (plugin plan section 12's B9 note).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSCounterFidelityChunkCountTest,
	"SuperFAISS.B.CounterFidelityChunkCount",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSCounterFidelityChunkCountTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	// Same 12000x96 int8 shape as the parallel-equality suite — enough chunks for a
	// real (>1) fan-out under forced-parallel mode.
	USuperFAISSVectorBank* Bank = MakeBank(*this, 12000, 96, ESuperFAISSBankMetric::Cosine,
		ESuperFAISSBankQuantization::Int8, 0xC0C0ull);
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
	Args.K = 10;

	const int32 ExpectedParallelChunks = superfaiss::ChunkCount(Bank->GetBankView());
	TestTrue(TEXT("fixture actually has >1 chunk (the setting that matters)"),
		ExpectedParallelChunks > 1);

	ModeVar->Set(0); // force serial: the actual partition is 1, not ChunkCount(view)
	{
		TArray<FSuperFAISSHit> Hits;
		TestTrue(TEXT("serial query ok"), Subsystem->QuerySync(Bank, MakeQuery(96, 10), Args, Hits));
		TestEqual(TEXT("serial: chunk count == 1 (the actual partition)"),
			Subsystem->GetLastQueryChunkCount(), 1);
	}

	ModeVar->Set(2); // force parallel: the actual partition is ChunkCount(view)
	{
		TArray<FSuperFAISSHit> Hits;
		TestTrue(TEXT("parallel query ok"), Subsystem->QuerySync(Bank, MakeQuery(96, 10), Args, Hits));
		TestEqual(TEXT("parallel: chunk count == ChunkCount(view) (the actual partition)"),
			Subsystem->GetLastQueryChunkCount(), ExpectedParallelChunks);
	}

	ModeVar->Set(SavedMode);
	return true;
}

// B9c: batch-size fidelity. The oracle is exactly M, the caller's QueryCount.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSCounterFidelityBatchSizeTest,
	"SuperFAISS.B.CounterFidelityBatchSize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSCounterFidelityBatchSizeTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	USuperFAISSVectorBank* Bank = MakeBank(*this, 2500, 48, ESuperFAISSBankMetric::Cosine,
		ESuperFAISSBankQuantization::Int8, 0xBA75ull);
	if (!Subsystem || !Bank)
	{
		return true;
	}

	FSuperFAISSQueryArgs Args;
	Args.K = 9;

	// Two distinct M values (including the odd-tail shape A5 exercises) so the counter
	// is proven to track the CALLER's M, not a fixed constant.
	for (const int32 QueryCount : {7, 3})
	{
		TArray<float> Queries;
		Queries.Reserve(QueryCount * 48);
		for (int32 M = 0; M < QueryCount; ++M)
		{
			Queries.Append(MakeQuery(48, 5000 + M));
		}
		TArray<FSuperFAISSHit> BatchHits;
		TArray<int32> BatchCounts;
		TestTrue(TEXT("batch ok"),
			Subsystem->QueryBatch(Bank, Queries, QueryCount, Args, BatchHits, BatchCounts));
		TestEqual(FString::Printf(TEXT("batch size counter == M (%d)"), QueryCount),
			Subsystem->GetLastQueryBatchSize(), QueryCount);
	}
	return true;
}

// The load-bearing code rule's mechanical half (plugin plan section 5.1, stated twice
// in the spec because it is the one that gates shipping): "no TRACE_*/stat-macro
// argument is a mutable reference into scan working memory... every stat counter is a
// write-only accumulator never read back into a kernel decision." B8 above is the
// EXECUTED proof; this is the reviewable GREP TARGET, the same "mechanical
// oracle rather than a review impression" shape as SuperFAISS.D.
// InspectorConcurrencyGrepTarget (audit N-5's precedent this cell is modeled on,
// verbatim per the plan's own "checked the same way the section 5.1 no-aliasing code
// rule is" cross-reference).
//
// The stat-set macros are wired at real query/batch/async/pool call sites (section
// 5.1 built); this cell scans them for real and its assertions are load-bearing, not
// vacuous. A STANDING regression guard for every future edit to these files.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInstrumentationNoMutableAliasGrepTargetTest,
	"SuperFAISS.B.InstrumentationNoMutableAliasGrepTarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInstrumentationNoMutableAliasGrepTargetTest::RunTest(const FString& Parameters)
{
	// FPaths, not IPluginManager (Projects module): the runtime module carries no
	// Projects dependency and section 5.1's rule concerns THIS module's own source, so
	// the project-relative plugin path is sufficient and adds no new module dependency.
	const FString ModuleDir = FPaths::Combine(
		FPaths::ProjectPluginsDir(), TEXT("SuperFAISSUnreal/Source/SuperFAISSUnreal"));

	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *ModuleDir, TEXT("*.cpp"), true, false, false);
	IFileManager::Get().FindFilesRecursive(Files, *ModuleDir, TEXT("*.h"), true, false, false);

	// The counter-SET macro families a stat group of this shape uses (DECLARE_CYCLE_
	// STAT's SCOPE_CYCLE_COUNTER takes no value argument — it is a scope marker, like
	// TRACE_CPUPROFILER_EVENT_SCOPE — so the aliasing hazard is scoped to macros that DO
	// take a value expression).
	const TCHAR* StatSetMacros[] = {
		TEXT("SET_DWORD_STAT("), TEXT("SET_FLOAT_STAT("), TEXT("SET_MEMORY_STAT("),
		TEXT("INC_DWORD_STAT_BY("), TEXT("INC_MEMORY_STAT_BY(")
	};
	// Scan working memory the rule protects: FPooledWorkspace's own members (the only
	// place per-query/per-chunk/merge state lives) plus the local View/query-count
	// names each RunQuery/QueryBatch/QueryIntersect/QueryScratch call site builds.
	const TCHAR* ScanWorkingMemoryIdentifiers[] = {
		TEXT("QueryStaging"), TEXT("HitStaging"), TEXT("CountStaging"),
		TEXT("ChunkHeapStorage"), TEXT("ChunkSorted"), TEXT("ChunkListPtrs"),
		TEXT("ChunkListCounts"), TEXT("MergeHeap"), TEXT("MergeCountScratch"),
		TEXT("TombstoneStaging"), TEXT("XdImageStaging"), TEXT("XdQueryStaging"),
		TEXT("Scratch.Core"), TEXT("Scratch->Core"), TEXT("&Scratch"), TEXT("&View")
	};

	int32 StatSetCallSitesFound = 0;
	for (const FString& File : Files)
	{
		// The claim is about PRODUCTION code, exactly as InspectorConcurrencyGrepTarget
		// excludes its own Tests/ tree — this file's own BannedSymbols-equivalent arrays
		// above would otherwise self-match.
		if (File.Contains(TEXT("/Tests/")) || File.Contains(TEXT("\\Tests\\"))) { continue; }
		FString Contents;
		if (!FFileHelper::LoadFileToString(Contents, *File)) { continue; }

		TArray<FString> Lines;
		Contents.ParseIntoArrayLines(Lines, false);
		for (const FString& Line : Lines)
		{
			bool bIsStatSet = false;
			for (const TCHAR* Macro : StatSetMacros)
			{
				if (Line.Contains(Macro)) { bIsStatSet = true; break; }
			}
			if (!bIsStatSet) { continue; }
			++StatSetCallSitesFound;
			for (const TCHAR* Id : ScanWorkingMemoryIdentifiers)
			{
				TestFalse(FString::Printf(
					TEXT("stat-set macro in %s must not alias scan working memory (%s): %s"),
					*File, Id, *Line),
					Line.Contains(Id));
			}
		}
	}
	// Discloses how many real call sites this pass scanned, so a reader confirms the
	// cell is exercising real code, not vacuous.
	AddInfo(FString::Printf(TEXT("instrumentation stat-set call sites scanned: %d"),
		StatSetCallSitesFound));
	// Enforced, not merely narrated: a grep that matched nothing would pass the loop
	// above vacuously despite the comment above claiming otherwise. This is the same
	// non-zero-count discipline the coverage audit's structural guards use.
	TestTrue(TEXT("instrumentation stat-set call sites scanned must be non-zero (grep target must not be vacuous)"),
		StatSetCallSitesFound > 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
