// V3.2 plan section 25.6's addendum on top of plugin plan section 5.1: three NAMED
// editor-side trace scopes on the Bank Inspector's own passes (structure build,
// novelty calibration, correspondence match), so a timing capture of those passes is
// an Insights read, not a stopwatch. Same read-only/non-perturbing rule as section
// 5.1's runtime-module markup; the passes already ride the instrumented query path
// underneath once the runtime-side scopes exist (SuperFAISSInstrumentationTests.cpp,
// runtime module).
//
// Coverage Model: section 25.9's M5 dimension states the one 3.2-new M5 cell exactly:
// "an Inspector pass run trace-ON produces bit-identical structure/novelty/
// correspondence outputs to trace-OFF (the B8 pattern extended over the three new
// editor scopes)". That is this file's entire scope — trace-scope EMISSION itself
// (asserting the three TRACE_CPUPROFILER_EVENT_SCOPE macros were typed) is explicitly
// NOT unit-testable and is instead part of the visual
// Insights smoke gate, owed to a human, not a cell here.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "SSuperFAISSBankInspector.h"
#include "SuperFAISSVectorBank.h"
#include "Trace/Trace.h"

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

	USuperFAISSVectorBank* MakeBank(FAutomationTestBase& Test, const TArray<float>& Rows,
		int32 Count, int32 Dims, ESuperFAISSBankMetric Metric, ESuperFAISSBankQuantization Quant)
	{
		USuperFAISSVectorBank* Bank = NewObject<USuperFAISSVectorBank>();
		FString Error;
		const bool bOk = Bank->InitFromSource(Rows, Count, Dims, Metric, Quant, {},
			TEXT("inspector-instrumentation-test"), Error);
		Test.TestTrue(FString::Printf(TEXT("bank built: %s"), *Error), bOk);
		return bOk ? Bank : nullptr;
	}

	bool StructureClustersEqual(const TArray<FSuperFAISSStructureCluster>& A,
		const TArray<FSuperFAISSStructureCluster>& B)
	{
		if (A.Num() != B.Num()) { return false; }
		for (int32 i = 0; i < A.Num(); ++i)
		{
			if (A[i].ComponentId != B[i].ComponentId) { return false; }
			if (A[i].MemberSampleIndices != B[i].MemberSampleIndices) { return false; }
		}
		return true;
	}

	bool MatchPairsEqual(const TArray<FSuperFAISSMatchPairResult>& A,
		const TArray<FSuperFAISSMatchPairResult>& B)
	{
		if (A.Num() != B.Num()) { return false; }
		for (int32 i = 0; i < A.Num(); ++i)
		{
			if (A[i].SourceIndexA != B[i].SourceIndexA) { return false; }
			if (A[i].SourceIndexB != B[i].SourceIndexB) { return false; }
			if (A[i].State != B[i].State) { return false; }
			uint32 BitsA, BitsB;
			FMemory::Memcpy(&BitsA, &A[i].CslsMargin, 4);
			FMemory::Memcpy(&BitsB, &B[i].CslsMargin, 4);
			if (BitsA != BitsB) { return false; }
		}
		return true;
	}
}

// The one 3.2-new M5 cell (section 25.9): each of the three named Inspector passes,
// run trace-OFF and trace-ON on the same inputs, produces bit-identical output state.
// Two distinct claims, matching SuperFAISS.B.InstrumentationNonPerturbation's shape:
//   1. The channel exists and toggles — RED-UNIMPLEMENTED today (the SuperFAISS trace
//      channel is not registered until the runtime-module half of this slot lands;
//      UE::Trace::ToggleChannel returns false for an unknown channel name).
//   2. Trace-OFF and trace-ON produce identical Structure/Novelty/Correspondence
//      state — GREEN AT AUTHORING TIME as a direct consequence of claim 1's scaffold
//      state (an unregistered channel's toggle is a no-op, so "trace-ON" runs
//      identically to "trace-OFF" today; there is nothing yet that could perturb a
//      pass). Standing regression guard: once the three named scopes are wired in
//      (riding the already-instrumented runtime query path underneath), this
//      assertion becomes the load-bearing B8-extension proof for the editor module.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSInspectorInstrumentationNonPerturbationTest,
	"SuperFAISS.D.InspectorInstrumentationNonPerturbation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSInspectorInstrumentationNonPerturbationTest::RunTest(const FString& Parameters)
{
	// Claim 1: the channel exists and toggles ON. Red today; shared with the runtime
	// module's cell, checked again here because THIS module must also see it exist
	// (it is a process-wide registry, but the claim belongs to whichever module
	// exercises the passes it gates).
	const bool bToggledOn = UE::Trace::ToggleChannel(TEXT("SuperFAISS"), true);
	TestTrue(TEXT("the SuperFAISS trace channel exists and enables"), bToggledOn);
	UE::Trace::ToggleChannel(TEXT("SuperFAISS"), false);

	// Structure: BankA shape ported from SuperFAISS.D.InspectorCacheInvalidationMatrix
	// (already proven there to give GetStructureClusters().Num() > 0), so the OFF/ON
	// comparison is over real, non-trivially-empty state rather than a vacuous match
	// of two empty lists.
	USuperFAISSVectorBank* StructureBank = MakeBank(*this, SeededRows(20, 8, 0x1A1A), 20, 8,
		ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Float32);
	if (StructureBank != nullptr)
	{
		UE::Trace::ToggleChannel(TEXT("SuperFAISS"), false);
		TSharedRef<SSuperFAISSBankInspector> Off = SNew(SSuperFAISSBankInspector);
		Off->SetBankForTest(StructureBank);
		Off->ComputeStructure();
		const TArray<FSuperFAISSStructureCluster> ClustersOff = Off->GetStructureClusters();
		const TArray<int32> OutliersOff = Off->GetStructureOutlierSampleIndices();
		const TArray<int32> ComponentIdsOff = Off->GetStructureComponentIdBySampleIndex();
		TestTrue(TEXT("(setup) Structure OFF pass produced real, non-empty state"),
			ClustersOff.Num() > 0);

		UE::Trace::ToggleChannel(TEXT("SuperFAISS"), true);
		TSharedRef<SSuperFAISSBankInspector> On = SNew(SSuperFAISSBankInspector);
		On->SetBankForTest(StructureBank);
		On->ComputeStructure();
		UE::Trace::ToggleChannel(TEXT("SuperFAISS"), false);

		TestTrue(TEXT("Structure: clusters identical trace-OFF vs trace-ON"),
			StructureClustersEqual(ClustersOff, On->GetStructureClusters()));
		TestEqual(TEXT("Structure: outlier sample indices identical trace-OFF vs trace-ON"),
			OutliersOff, On->GetStructureOutlierSampleIndices());
		TestEqual(TEXT("Structure: component-id-by-sample-index identical trace-OFF vs trace-ON"),
			ComponentIdsOff, On->GetStructureComponentIdBySampleIndex());
	}

	// Novelty: same bank/probe pattern already proven to give bValid == true (the
	// panel tests' own "#0" probe convention).
	USuperFAISSVectorBank* NoveltyBank = MakeBank(*this, SeededRows(30, 12, 0x50A1), 30, 12,
		ESuperFAISSBankMetric::L2, ESuperFAISSBankQuantization::Float32);
	if (NoveltyBank != nullptr)
	{
		UE::Trace::ToggleChannel(TEXT("SuperFAISS"), false);
		TSharedRef<SSuperFAISSBankInspector> Off = SNew(SSuperFAISSBankInspector);
		Off->SetBankForTest(NoveltyBank);
		Off->ProbeNovelty(TEXT("#5"));
		const FSuperFAISSNoveltyResult ResultOff = Off->GetNoveltyResult();
		TestTrue(TEXT("(setup) Novelty OFF pass produced a valid verdict"), ResultOff.bValid);

		UE::Trace::ToggleChannel(TEXT("SuperFAISS"), true);
		TSharedRef<SSuperFAISSBankInspector> On = SNew(SSuperFAISSBankInspector);
		On->SetBankForTest(NoveltyBank);
		On->ProbeNovelty(TEXT("#5"));
		UE::Trace::ToggleChannel(TEXT("SuperFAISS"), false);
		const FSuperFAISSNoveltyResult ResultOn = On->GetNoveltyResult();

		TestEqual(TEXT("Novelty: bValid identical trace-OFF vs trace-ON"),
			ResultOn.bValid, ResultOff.bValid);
		TestTrue(TEXT("Novelty: verdict identical trace-OFF vs trace-ON"),
			ResultOn.Verdict == ResultOff.Verdict);
		uint32 ScoreBitsOff, ScoreBitsOn;
		FMemory::Memcpy(&ScoreBitsOff, &ResultOff.Score, 4);
		FMemory::Memcpy(&ScoreBitsOn, &ResultOn.Score, 4);
		TestEqual(TEXT("Novelty: score bits identical trace-OFF vs trace-ON"),
			ScoreBitsOn, ScoreBitsOff);
		TestEqual(TEXT("Novelty: bLowConfidence identical trace-OFF vs trace-ON"),
			ResultOn.bLowConfidence, ResultOff.bLowConfidence);
		TestEqual(TEXT("Novelty: sampled/total counts identical trace-OFF vs trace-ON"),
			ResultOn.SampledCount == ResultOff.SampledCount &&
				ResultOn.TotalCount == ResultOff.TotalCount, true);
	}

	// Correspondence: the Primary/CompatibleSecond pair ported from
	// SuperFAISS.D.InspectorCorrespondenceLateRejectionClearsList (already proven
	// there to give GetMatchPairResults().Num() > 0).
	USuperFAISSVectorBank* Primary = MakeBank(*this, SeededRows(20, 8, 0xC0B1), 20, 8,
		ESuperFAISSBankMetric::L2, ESuperFAISSBankQuantization::Float32);
	USuperFAISSVectorBank* CompatibleSecond = MakeBank(*this, SeededRows(20, 8, 0xC0B2), 20, 8,
		ESuperFAISSBankMetric::L2, ESuperFAISSBankQuantization::Float32);
	if (Primary != nullptr && CompatibleSecond != nullptr)
	{
		UE::Trace::ToggleChannel(TEXT("SuperFAISS"), false);
		TSharedRef<SSuperFAISSBankInspector> Off = SNew(SSuperFAISSBankInspector);
		Off->SetBankForTest(Primary);
		Off->SetSecondBankForTest(CompatibleSecond);
		Off->ComputeCorrespondence();
		const TArray<FSuperFAISSMatchPairResult> PairsOff = Off->GetMatchPairResults();
		const FString StatusOff = Off->GetCorrespondenceStatus();
		TestTrue(TEXT("(setup) Correspondence OFF pass produced real, non-empty state"),
			PairsOff.Num() > 0);

		UE::Trace::ToggleChannel(TEXT("SuperFAISS"), true);
		TSharedRef<SSuperFAISSBankInspector> On = SNew(SSuperFAISSBankInspector);
		On->SetBankForTest(Primary);
		On->SetSecondBankForTest(CompatibleSecond);
		On->ComputeCorrespondence();
		UE::Trace::ToggleChannel(TEXT("SuperFAISS"), false);

		TestTrue(TEXT("Correspondence: match pairs identical trace-OFF vs trace-ON"),
			MatchPairsEqual(PairsOff, On->GetMatchPairResults()));
		TestEqual(TEXT("Correspondence: status identical trace-OFF vs trace-ON"),
			On->GetCorrespondenceStatus(), StatusOff);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
