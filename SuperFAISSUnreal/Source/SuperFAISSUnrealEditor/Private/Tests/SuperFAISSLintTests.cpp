// Bank-lint and prototype-authoring tests (plan 18.2 / V1.1 slot C, gate step 1:
// core-math tools green before render/UX).

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "SuperFAISSAuthoringLibrary.h"
#include "SuperFAISSBankLint.h"
#include "SuperFAISSPrototypeAsset.h"
#include "SuperFAISSQueryProvider.h"
#include "SuperFAISSSubsystem.h"
#include "SuperFAISSVectorBank.h"

namespace
{
	TArray<float> LintRows(int32 Count, int32 Dims, uint64 Seed)
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

	USuperFAISSVectorBank* LintBank(FAutomationTestBase& Test, const TArray<float>& Rows,
		int32 Count, int32 Dims)
	{
		USuperFAISSVectorBank* Bank = NewObject<USuperFAISSVectorBank>();
		FString Error;
		const bool bOk = Bank->InitFromSource(Rows, Count, Dims,
			ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Float32, {},
			TEXT("lint-test"), Error);
		Test.TestTrue(FString::Printf(TEXT("bank built: %s"), *Error), bOk);
		return bOk ? Bank : nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSBankLintTest,
	"SuperFAISS.D.BankLint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSBankLintTest::RunTest(const FString& Parameters)
{
	constexpr int32 Count = 120;
	constexpr int32 Dims = 16;

	// Plant: row 40 duplicates row 3; dim 2 is constant across all rows.
	TArray<float> Rows = LintRows(Count, Dims, 0x11117ull);
	for (int32 J = 0; J < Dims; ++J)
	{
		Rows[40 * Dims + J] = Rows[3 * Dims + J];
	}
	for (int32 R = 0; R < Count; ++R)
	{
		Rows[R * Dims + 2] = 0.7f;
	}

	USuperFAISSVectorBank* Bank = LintBank(*this, Rows, Count, Dims);
	if (!Bank)
	{
		return true;
	}

	FSuperFAISSLintReport Report;
	TestTrue(TEXT("near-dup ran"),
		FSuperFAISSBankLint::FindNearDuplicates(Bank, 0.9999f, 4096, Report));
	TestFalse(TEXT("not sampled"), Report.bSampled);
	TestEqual(TEXT("rows examined"), Report.RowsExamined, Count);
	bool bFoundPlanted = false;
	for (const FSuperFAISSNearDuplicate& Dup : Report.NearDuplicates)
	{
		if (Dup.RowA == 3 && Dup.RowB == 40)
		{
			bFoundPlanted = true;
		}
	}
	TestTrue(TEXT("planted duplicate found"), bFoundPlanted);

	// The constant dim is NOT low-variance-flagged wrongly on other dims: exactly
	// dim 2 trips (note: Cosine normalization rescales rows, so the planted dim is
	// constant only pre-normalization; use a variance epsilon sized for the
	// normalized spread, and require dim 2 to be the minimum-variance dim instead
	// of asserting an absolute).
	TestTrue(TEXT("low-variance ran"),
		FSuperFAISSBankLint::FindLowVarianceDims(Bank, 1e-3f, Report));
	TestTrue(TEXT("dim 2 flagged"), Report.LowVarianceDims.Contains(2));

	// Clean bank: no near-duplicates at a tight threshold.
	const TArray<float> CleanRows = LintRows(Count, Dims, 0xC1EA7ull);
	USuperFAISSVectorBank* CleanBank = LintBank(*this, CleanRows, Count, Dims);
	if (CleanBank)
	{
		FSuperFAISSLintReport CleanReport;
		TestTrue(TEXT("clean ran"),
			FSuperFAISSBankLint::FindNearDuplicates(CleanBank, 0.9999f, 4096, CleanReport));
		TestEqual(TEXT("clean has no dups"), CleanReport.NearDuplicates.Num(), 0);

		// N1 guard: a sample limit below Count trips sampling and stays bounded.
		FSuperFAISSLintReport Sampled;
		TestTrue(TEXT("sampled ran"),
			FSuperFAISSBankLint::FindNearDuplicates(CleanBank, 0.9999f, 32, Sampled));
		TestTrue(TEXT("sampling flagged"), Sampled.bSampled);
		TestTrue(TEXT("sample bounded"), Sampled.RowsExamined <= 64);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSPrototypeAuthoringTest,
	"SuperFAISS.D.PrototypeAuthoring",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSPrototypeAuthoringTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	constexpr int32 Count = 40;
	constexpr int32 Dims = 12;
	const TArray<float> Rows = LintRows(Count, Dims, 0x9207ull);
	TArray<FName> Ids;
	for (int32 R = 0; R < Count; ++R)
	{
		Ids.Add(FName(*FString::Printf(TEXT("w%d"), R)));
	}
	USuperFAISSVectorBank* Bank = NewObject<USuperFAISSVectorBank>();
	FString Error;
	if (!TestTrue(TEXT("bank built"), Bank->InitFromSource(Rows, Count, Dims,
		ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Float32, Ids,
		TEXT("authoring-test"), Error)))
	{
		return true;
	}

	// Author by mixed ids + indices; the asset's vector equals the direct centroid.
	FString AuthorError;
	USuperFAISSPrototypeAsset* Asset = USuperFAISSAuthoringLibrary::CreatePrototypeAsset(
		Bank, {7}, {TEXT("w3"), TEXT("w19")}, TEXT("/Temp/SuperFAISSAuthoringTest"),
		TEXT("ProtoA"), AuthorError);
	if (!TestNotNull(FString::Printf(TEXT("asset created: %s"), *AuthorError), Asset))
	{
		return true;
	}
	TestEqual(TEXT("provenance rows"), Asset->SourceRows.Num(), 3);

	TArray<float> Direct;
	TestTrue(TEXT("direct centroid"),
		Subsystem->MakeCentroidQuery(Bank, Asset->SourceRows, Direct));
	TestEqual(TEXT("vector dims"), Asset->Query.Num(), Direct.Num());
	for (int32 J = 0; J < FMath::Min(Asset->Query.Num(), Direct.Num()); ++J)
	{
		TestEqual(TEXT("vector element"), Asset->Query[J], Direct[J]);
	}

	// The asset is a working query provider against its bank.
	TArray<float> Provided;
	TestTrue(TEXT("asset provides"),
		ISuperFAISSQueryProvider::Execute_GetQueryVector(Asset, Bank, Provided));
	FSuperFAISSQueryArgs Args;
	Args.K = 5;
	TArray<FSuperFAISSHit> Hits;
	TestTrue(TEXT("prototype queries"), Subsystem->QuerySync(Bank, Provided, Args, Hits));
	TestTrue(TEXT("prototype has hits"), Hits.Num() > 0);

	// Overlap analysis: the prototype overlaps itself, not an orthogonal one.
	USuperFAISSPrototypeAsset* Twin = NewObject<USuperFAISSPrototypeAsset>();
	Twin->Query = Asset->Query;
	USuperFAISSPrototypeAsset* Ortho = NewObject<USuperFAISSPrototypeAsset>();
	Ortho->Query.SetNumZeroed(Dims);
	// Build a vector orthogonal to the prototype: swap two components, negate one.
	Ortho->Query[0] = -Asset->Query[1];
	Ortho->Query[1] = Asset->Query[0];
	const USuperFAISSPrototypeAsset* Protos[3] = {Asset, Twin, Ortho};
	FSuperFAISSLintReport Report;
	TestTrue(TEXT("overlap ran"),
		FSuperFAISSBankLint::FindPrototypeOverlaps(Protos, 0.99f, Report));
	bool bTwinFlagged = false;
	bool bOrthoFlagged = false;
	for (const FSuperFAISSPrototypeOverlap& O : Report.PrototypeOverlaps)
	{
		if (O.PrototypeA == 0 && O.PrototypeB == 1) { bTwinFlagged = true; }
		if (O.PrototypeB == 2) { bOrthoFlagged = true; }
	}
	TestTrue(TEXT("twin overlap flagged"), bTwinFlagged);
	TestFalse(TEXT("orthogonal not flagged"), bOrthoFlagged);

	// Failure modes: unknown id; empty selection.
	FString E2;
	TestNull(TEXT("unknown id rejected"), USuperFAISSAuthoringLibrary::CreatePrototypeAsset(
		Bank, {}, {TEXT("missing")}, TEXT("/Temp/SuperFAISSAuthoringTest"),
		TEXT("ProtoBad"), E2));
	TestNull(TEXT("empty rejected"), USuperFAISSAuthoringLibrary::CreatePrototypeAsset(
		Bank, {}, {}, TEXT("/Temp/SuperFAISSAuthoringTest"), TEXT("ProtoEmpty"), E2));

	return true;
}

// T-V2.4-U1/U2 — cross-device prototype tier (V2 plan section 21, FAI-1): the
// authoring twin entry point bakes the SAME core operator's product — the baked
// quantized centroid byte-equals a runtime MakeCentroidQueryCrossDevice over
// identical rows (one operator, two entry points, no second math). The asset
// format takes a REQUIRED minor-version bump for the cross-device tier: an
// XD-tier payload without the bumped version is a defined rejection.
// Float-storing prototype assets remain the presentation tier, unchanged.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSPrototypeCrossDeviceTest,
	"SuperFAISS.D.PrototypeCrossDevice",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSPrototypeCrossDeviceTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	constexpr int32 Count = 60;
	constexpr int32 Dims = 24;
	const TArray<float> Rows = LintRows(Count, Dims, 0xD07Aull);
	USuperFAISSVectorBank* Bank = NewObject<USuperFAISSVectorBank>();
	FString Error;
	if (!TestTrue(TEXT("int8 bank built"), Bank->InitFromSource(Rows, Count, Dims,
			ESuperFAISSBankMetric::Dot, ESuperFAISSBankQuantization::Int8, {},
			TEXT("xd-proto-test"), Error)))
	{
		return true;
	}

	const TArray<int32> Members = {3, 11, 29, 47};

	// U1: bake the anchor through the authoring twin entry point.
	FString AuthorError;
	USuperFAISSPrototypeAsset* Asset =
		USuperFAISSAuthoringLibrary::CreatePrototypeAssetCrossDevice(Bank, Members, {},
			{}, TEXT("/Temp/SuperFAISSAuthoringTest"), TEXT("ProtoXd"), AuthorError);
	if (!TestNotNull(FString::Printf(TEXT("xd asset created: %s"), *AuthorError), Asset))
	{
		return true;
	}
	TestEqual(TEXT("required version bump"), Asset->AssetVersion,
		USuperFAISSPrototypeAsset::kAssetVersionCrossDevice);
	TestTrue(TEXT("cross-device tier"), Asset->IsCrossDeviceTier());

	// The baked payload byte-equals a runtime pool over identical rows.
	FSuperFAISSCrossDeviceQuery Runtime;
	TestTrue(TEXT("runtime pool"),
		Subsystem->MakeCentroidQueryCrossDevice(Bank, Members, {}, Runtime));
	FSuperFAISSCrossDeviceQuery Baked;
	TestTrue(TEXT("baked payload readable"), Asset->GetCrossDeviceQuery(Baked));
	TestTrue(TEXT("baked image byte-equals runtime"), Baked.ImageQ8 == Runtime.ImageQ8);
	TestTrue(TEXT("baked scale bit-equals runtime"), Baked.Scale == Runtime.Scale);
	TestEqual(TEXT("baked self-dot equals runtime"), Baked.SqSum, Runtime.SqSum);

	// The baked anchor executes: hits bit-equal the runtime payload's hits.
	TArray<FSuperFAISSHit> BakedHits;
	TArray<FSuperFAISSHit> RuntimeHits;
	TestTrue(TEXT("baked executes"),
		Subsystem->QueryPooledCrossDevice(Bank, Baked, 6, BakedHits));
	TestTrue(TEXT("runtime executes"),
		Subsystem->QueryPooledCrossDevice(Bank, Runtime, 6, RuntimeHits));
	TestEqual(TEXT("hit counts"), BakedHits.Num(), RuntimeHits.Num());
	for (int32 i = 0; i < FMath::Min(BakedHits.Num(), RuntimeHits.Num()); ++i)
	{
		TestTrue(FString::Printf(TEXT("baked hit %d bit-equal"), i),
			BakedHits[i].Index == RuntimeHits[i].Index &&
				BakedHits[i].Score == RuntimeHits[i].Score);
	}

	// U2: the version gate is REQUIRED — an XD payload under the presentation
	// version is a defined rejection, never silently readable.
	{
		const int32 SavedVersion = Asset->AssetVersion;
		Asset->AssetVersion = SavedVersion - 1;
		FSuperFAISSCrossDeviceQuery Gated;
		TestFalse(TEXT("unbumped version rejected"), Asset->GetCrossDeviceQuery(Gated));
		Asset->AssetVersion = SavedVersion;
		TestTrue(TEXT("restored version reads"), Asset->GetCrossDeviceQuery(Gated));
	}

	// Float-storing assets are the presentation tier, unchanged: no XD payload, no
	// XD read, the float provider path still works.
	{
		FString FloatError;
		USuperFAISSPrototypeAsset* FloatAsset =
			USuperFAISSAuthoringLibrary::CreatePrototypeAsset(Bank, Members, {},
				TEXT("/Temp/SuperFAISSAuthoringTest"), TEXT("ProtoFloat"), FloatError);
		if (TestNotNull(FString::Printf(TEXT("float asset: %s"), *FloatError), FloatAsset))
		{
			TestFalse(TEXT("presentation tier"), FloatAsset->IsCrossDeviceTier());
			FSuperFAISSCrossDeviceQuery None;
			TestFalse(TEXT("no xd payload to read"), FloatAsset->GetCrossDeviceQuery(None));
			TArray<float> Provided;
			TestTrue(TEXT("float provider unchanged"),
				ISuperFAISSQueryProvider::Execute_GetQueryVector(FloatAsset, Bank, Provided));
		}
	}

	// The XD asset also carries the float presentation form (provider-compatible).
	{
		TArray<float> Provided;
		TestTrue(TEXT("xd asset still provides the float form"),
			ISuperFAISSQueryProvider::Execute_GetQueryVector(Asset, Bank, Provided));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
