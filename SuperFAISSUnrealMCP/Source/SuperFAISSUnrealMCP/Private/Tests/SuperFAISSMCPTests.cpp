// M1/M2 (plan §19.5): the MCP tool handlers are plain reflected statics, so the suite
// tests them directly — golden outputs against the demo bank, an ImportBank fixture
// round-trip with the W3 overwrite contract, and the argument-validation matrix. The
// protocol layer is Epic's to test; the live-client half is M-V1 (build log).

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Engine/Engine.h"
#include "SuperFAISSToolset.h"
#include "SuperFAISSScratchBank.h"
#include "SuperFAISSSubsystem.h"
#include "SuperFAISSVectorBank.h"
#include "UObject/Package.h"

namespace
{
	TSharedPtr<FJsonObject> ParseTool(const FString& Result)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Result);
		FJsonSerializer::Deserialize(Reader, Object);
		return Object;
	}

	bool IsToolError(const FString& Result)
	{
		const TSharedPtr<FJsonObject> Object = ParseTool(Result);
		return Object.IsValid() && Object->HasField(TEXT("error"));
	}

	// Sidecar fixture: Count x Dims float32 rows with per-row ids row0..rowN.
	struct FMCPFixture
	{
		FString JsonPath;

		FMCPFixture(const FString& Name, int32 Count, int32 Dims, bool bWithIds)
		{
			const FString Dir =
				FPaths::ProjectIntermediateDir() / TEXT("SuperFAISSMCPTests");
			IFileManager::Get().MakeDirectory(*Dir, true);
			JsonPath = Dir / (Name + TEXT(".wvbank.json"));

			FString Json = FString::Printf(
				TEXT("{\n \"schemaVersion\": 1,\n \"dims\": %d,\n \"count\": %d,\n")
				TEXT(" \"metric\": \"cosine\",\n \"dtype\": \"float32\""), Dims, Count);
			if (bWithIds)
			{
				Json += TEXT(",\n \"ids\": [");
				for (int32 i = 0; i < Count; ++i)
				{
					Json += FString::Printf(TEXT("%s\"row%d\""), i ? TEXT(", ") : TEXT(""), i);
				}
				Json += TEXT("]");
			}
			Json += TEXT("\n}\n");
			FFileHelper::SaveStringToFile(Json, *JsonPath);

			TArray<float> Rows;
			Rows.SetNumUninitialized(Count * Dims);
			uint64 State = 0xF1C7ull + Count;
			for (float& V : Rows)
			{
				State ^= State >> 12;
				State ^= State << 25;
				State ^= State >> 27;
				V = static_cast<float>(static_cast<int64>((State * 0x2545F4914F6CDD1Dull) >> 24)) /
					static_cast<float>(1ll << 39);
			}
			const TArrayView<const uint8> Bytes(
				reinterpret_cast<const uint8*>(Rows.GetData()), Rows.Num() * sizeof(float));
			FFileHelper::SaveArrayToFile(Bytes, *(JsonPath.LeftChop(5) + TEXT(".bin")));
		}
	};

	const TCHAR* DemoBankPath = TEXT("/SuperFAISSUnreal/Demo/DemoBank.DemoBank");
	const TCHAR* ImportDest = TEXT("/Game/SuperFAISSMCPTests/ToolImport");
	const TCHAR* ImportDestIdless = TEXT("/Game/SuperFAISSMCPTests/ToolImportIdless");

	void CleanupImportedAsset()
	{
		const FString FileName = FPackageName::LongPackageNameToFilename(
			ImportDest, FPackageName::GetAssetPackageExtension());
		IFileManager::Get().Delete(*FileName, false, true, true);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSMCPToolGoldensTest,
	"SuperFAISS.M.ToolGoldens",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSMCPToolGoldensTest::RunTest(const FString& Parameters)
{
	// Echo — the connectivity probe answers and names its thread.
	TestTrue(TEXT("echo"), USuperFAISSToolset::Echo(TEXT("x")).Contains(TEXT("echo: x")));

	// ListBanks finds the demo bank with its shipped shape.
	{
		const TSharedPtr<FJsonObject> R = ParseTool(USuperFAISSToolset::ListBanks());
		if (TestTrue(TEXT("ListBanks parses"), R.IsValid()))
		{
			bool bFoundDemo = false;
			for (const TSharedPtr<FJsonValue>& V : R->GetArrayField(TEXT("banks")))
			{
				const TSharedPtr<FJsonObject> B = V->AsObject();
				if (B->GetStringField(TEXT("path")) == DemoBankPath)
				{
					bFoundDemo = true;
					TestEqual(TEXT("demo count"), (int32)B->GetNumberField(TEXT("count")), 40000);
					TestEqual(TEXT("demo dims"), (int32)B->GetNumberField(TEXT("dims")), 100);
					TestEqual(TEXT("demo metric"),
						B->GetStringField(TEXT("metric")), FString(TEXT("Cosine")));
				}
			}
			TestTrue(TEXT("demo bank listed"), bFoundDemo);
		}
	}

	// DescribeBank: ids present, recall recorded.
	{
		const TSharedPtr<FJsonObject> R =
			ParseTool(USuperFAISSToolset::DescribeBank(DemoBankPath));
		if (TestTrue(TEXT("DescribeBank parses"), R.IsValid()))
		{
			TestTrue(TEXT("hasIds"), R->GetBoolField(TEXT("hasIds")));
			TestTrue(TEXT("recall recorded"),
				R->GetNumberField(TEXT("recallAt10")) > 0.9);
		}
	}

	// QueryBank by id: the F1 golden neighborhood through the tool path.
	{
		const TSharedPtr<FJsonObject> R = ParseTool(USuperFAISSToolset::QueryBank(
			DemoBankPath, TEXT("wizard"), -1, {}, {}, {}, {}, {}, 5, false));
		if (TestTrue(TEXT("QueryBank parses"), R.IsValid()) &&
			TestFalse(TEXT("QueryBank not error"), R->HasField(TEXT("error"))))
		{
			const TArray<TSharedPtr<FJsonValue>>& Hits = R->GetArrayField(TEXT("hits"));
			TestEqual(TEXT("k hits"), Hits.Num(), 5);
			TArray<FString> Ids;
			for (const TSharedPtr<FJsonValue>& V : Hits)
			{
				Ids.Add(V->AsObject()->GetStringField(TEXT("id")));
			}
			TestEqual(TEXT("self first"), Ids[0], FString(TEXT("wizard")));
			TestTrue(TEXT("magician in top-5"), Ids.Contains(TEXT("magician")));
			TestTrue(TEXT("sorcerer in top-5"), Ids.Contains(TEXT("sorcerer")));
			TestTrue(TEXT("margins present"),
				Hits[0]->AsObject()->HasField(TEXT("margin")));
		}
	}

	// QueryPrototype from two ids returns ranked hits.
	{
		const TSharedPtr<FJsonObject> R = ParseTool(USuperFAISSToolset::QueryPrototype(
			DemoBankPath, {TEXT("wizard"), TEXT("witch")}, {}, FString(), 5));
		if (TestTrue(TEXT("QueryPrototype parses"), R.IsValid()))
		{
			TestFalse(TEXT("prototype not error"), R->HasField(TEXT("error")));
			TestEqual(TEXT("prototype hits"),
				R->GetArrayField(TEXT("hits")).Num(), 5);
		}
	}

	// ImportBank fixture round-trip, with the W3 overwrite contract.
	{
		CleanupImportedAsset();
		FMCPFixture Fixture(TEXT("tool_roundtrip"), 8, 4, /*bWithIds*/ true);
		const TSharedPtr<FJsonObject> R = ParseTool(USuperFAISSToolset::ImportBank(
			Fixture.JsonPath, ImportDest, TEXT("Float32"), false));
		if (TestTrue(TEXT("import parses"), R.IsValid()) &&
			TestFalse(TEXT("import ok"), R->HasField(TEXT("error"))))
		{
			TestEqual(TEXT("imported count"), (int32)R->GetNumberField(TEXT("count")), 8);
			TestEqual(TEXT("imported dims"), (int32)R->GetNumberField(TEXT("dims")), 4);

			// Second import without overwrite is refused; with overwrite it lands.
			TestTrue(TEXT("collision refused"), IsToolError(USuperFAISSToolset::ImportBank(
				Fixture.JsonPath, ImportDest, TEXT("Float32"), false)));
			TestFalse(TEXT("overwrite allowed"), IsToolError(USuperFAISSToolset::ImportBank(
				Fixture.JsonPath, ImportDest, TEXT("Float32"), true)));

			// The imported bank answers queries through the tool path.
			const TSharedPtr<FJsonObject> Q = ParseTool(USuperFAISSToolset::QueryBank(
				FString(ImportDest) + TEXT(".ToolImport"), TEXT("row3"), -1, {}, {}, {}, {}, {}, 3, false));
			TestTrue(TEXT("imported bank queries"),
				Q.IsValid() && !Q->HasField(TEXT("error")));
		}
		CleanupImportedAsset();
	}

	// ValidateBanks: the shipped content banks are valid. (A suite run leaves
	// deliberately-corrupted transient banks loaded from earlier rejection tests, so
	// global-zero is not assertable here; no invalid bank may be a content asset.)
	{
		const TSharedPtr<FJsonObject> R = ParseTool(USuperFAISSToolset::ValidateBanks());
		if (TestTrue(TEXT("validate parses"), R.IsValid()))
		{
			for (const TSharedPtr<FJsonValue>& V : R->GetArrayField(TEXT("invalidBanks")))
			{
				const FString Path = V->AsString();
				TestFalse(FString::Printf(TEXT("content bank invalid: %s"), *Path),
					Path.StartsWith(TEXT("/SuperFAISSUnreal/")) ||
					Path.StartsWith(TEXT("/Game/")));
			}
		}
	}

	// Bias through the tool path (v2.1): a sparse reward lifts a named row into
	// the top-k of the golden query.
	{
		const TSharedPtr<FJsonObject> R = ParseTool(USuperFAISSToolset::QueryBank(
			DemoBankPath, TEXT("wizard"), -1, {}, {}, {}, {31337}, {100.0f}, 5, false));
		if (TestTrue(TEXT("bias query parses"), R.IsValid()) &&
			TestFalse(TEXT("bias query not error"), R->HasField(TEXT("error"))))
		{
			const TArray<TSharedPtr<FJsonValue>>& Hits = R->GetArrayField(TEXT("hits"));
			TestEqual(TEXT("bias hits"), Hits.Num(), 5);
			if (Hits.Num() > 0)
			{
				TestEqual(TEXT("biased row wins"),
					(int32)Hits[0]->AsObject()->GetNumberField(TEXT("index")), 31337);
			}
		}
	}

	// Scratch tools (D-M3, read-only): a live scratch bank is listed, described,
	// and queried through the tool path.
	{
		USuperFAISSScratchBank* Scratch = NewObject<USuperFAISSScratchBank>();
		TestTrue(TEXT("scratch init"), Scratch->Init(8, 4,
			ESuperFAISSBankMetric::Dot, ESuperFAISSBankQuantization::Float32));
		int32 Index = INDEX_NONE;
		TestTrue(TEXT("scratch append"),
			Scratch->Append({1.0f, 0.0f, 0.0f, 0.0f}, Index));
		TestTrue(TEXT("scratch append 2"),
			Scratch->Append({0.0f, 1.0f, 0.0f, 0.0f}, Index));

		const TSharedPtr<FJsonObject> L = ParseTool(USuperFAISSToolset::ListScratchBanks());
		bool bListed = false;
		if (TestTrue(TEXT("list scratch parses"), L.IsValid()))
		{
			for (const TSharedPtr<FJsonValue>& V : L->GetArrayField(TEXT("scratchBanks")))
			{
				if (V->AsObject()->GetStringField(TEXT("path")) == Scratch->GetPathName())
				{
					bListed = true;
				}
			}
		}
		TestTrue(TEXT("scratch bank listed"), bListed);

		const TSharedPtr<FJsonObject> D =
			ParseTool(USuperFAISSToolset::DescribeScratchBank(Scratch->GetPathName()));
		if (TestTrue(TEXT("describe scratch parses"), D.IsValid()))
		{
			TestEqual(TEXT("scratch count"), (int32)D->GetNumberField(TEXT("count")), 2);
			TestEqual(TEXT("scratch capacity"),
				(int32)D->GetNumberField(TEXT("capacity")), 8);
		}

		const TSharedPtr<FJsonObject> Q = ParseTool(USuperFAISSToolset::QueryScratchBank(
			Scratch->GetPathName(), {1.0f, 0.0f, 0.0f, 0.0f}, 2));
		if (TestTrue(TEXT("query scratch parses"), Q.IsValid()) &&
			TestFalse(TEXT("query scratch not error"), Q->HasField(TEXT("error"))))
		{
			const TArray<TSharedPtr<FJsonValue>>& Hits = Q->GetArrayField(TEXT("hits"));
			TestEqual(TEXT("scratch hits"), Hits.Num(), 2);
			if (Hits.Num() == 2)
			{
				TestEqual(TEXT("scratch best"),
					(int32)Hits[0]->AsObject()->GetNumberField(TEXT("index")), 0);
			}
		}
	}

	// LintBank on the demo bank, sampled (the N1 bound holds through the tool).
	{
		const TSharedPtr<FJsonObject> R = ParseTool(USuperFAISSToolset::LintBank(
			DemoBankPath, 0.9999f, 64, 0.00000001f));
		if (TestTrue(TEXT("lint parses"), R.IsValid()) &&
			TestFalse(TEXT("lint not error"), R->HasField(TEXT("error"))))
		{
			TestTrue(TEXT("lint sampled"), R->GetBoolField(TEXT("sampled")));
			TestTrue(TEXT("lint bounded"),
				(int32)R->GetNumberField(TEXT("rowsExamined")) <= 128);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSMCPToolValidationTest,
	"SuperFAISS.M.ToolValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSMCPToolValidationTest::RunTest(const FString& Parameters)
{
	// Every bad input returns a tool-level error object — never a crash, never a NaN.
	TestTrue(TEXT("unknown bank"), IsToolError(
		USuperFAISSToolset::DescribeBank(TEXT("/Game/DoesNotExist.DoesNotExist"))));
	TestTrue(TEXT("unknown bank query"), IsToolError(USuperFAISSToolset::QueryBank(
		TEXT("/Game/DoesNotExist.DoesNotExist"), TEXT("x"), -1, {}, {}, {}, {}, {}, 5, false)));
	TestTrue(TEXT("unknown id"), IsToolError(USuperFAISSToolset::QueryBank(
		DemoBankPath, TEXT("zzz_not_a_word_zzz"), -1, {}, {}, {}, {}, {}, 5, false)));
	TestTrue(TEXT("two query sources"), IsToolError(USuperFAISSToolset::QueryBank(
		DemoBankPath, TEXT("wizard"), 5, {}, {}, {}, {}, {}, 5, false)));
	TestTrue(TEXT("no query source"), IsToolError(USuperFAISSToolset::QueryBank(
		DemoBankPath, FString(), -1, {}, {}, {}, {}, {}, 5, false)));
	TestTrue(TEXT("bad vector dims"), IsToolError(USuperFAISSToolset::QueryBank(
		DemoBankPath, FString(), -1, {1.0f, 2.0f}, {}, {}, {}, {}, 5, false)));
	TestTrue(TEXT("row out of range"), IsToolError(USuperFAISSToolset::QueryBank(
		DemoBankPath, FString(), 999999, {}, {}, {}, {}, {}, 5, false)));

	// Bias args (v2.1): mismatched parallel arrays, out-of-range and duplicate
	// indices, and non-finite values are tool errors.
	TestTrue(TEXT("bias array mismatch"), IsToolError(USuperFAISSToolset::QueryBank(
		DemoBankPath, TEXT("wizard"), -1, {}, {}, {}, {0}, {}, 5, false)));
	TestTrue(TEXT("bias index out of range"), IsToolError(USuperFAISSToolset::QueryBank(
		DemoBankPath, TEXT("wizard"), -1, {}, {}, {}, {999999999}, {1.0f}, 5, false)));
	TestTrue(TEXT("bias duplicate index"), IsToolError(USuperFAISSToolset::QueryBank(
		DemoBankPath, TEXT("wizard"), -1, {}, {}, {}, {3, 3}, {1.0f, 2.0f}, 5, false)));

	// Channel args: mismatched parallel arrays and unknown channels are tool errors.
	TestTrue(TEXT("channel array mismatch"), IsToolError(USuperFAISSToolset::QueryBank(
		DemoBankPath, TEXT("wizard"), -1, {}, {TEXT("identity")}, {}, {}, {}, 5, false)));
	TestTrue(TEXT("unknown channel"), IsToolError(USuperFAISSToolset::QueryBank(
		DemoBankPath, TEXT("wizard"), -1, {}, {TEXT("identity")}, {1.0f}, {}, {}, 5, false)));

	// Scratch tools: unknown path and bad vector are tool errors, never crashes.
	TestTrue(TEXT("unknown scratch bank"), IsToolError(
		USuperFAISSToolset::DescribeScratchBank(TEXT("/Engine/Transient.Nope"))));
	TestTrue(TEXT("unknown scratch query"), IsToolError(
		USuperFAISSToolset::QueryScratchBank(TEXT("/Engine/Transient.Nope"), {1.0f}, 3)));
	{
		USuperFAISSScratchBank* Scratch = NewObject<USuperFAISSScratchBank>();
		TestTrue(TEXT("scratch v init"), Scratch->Init(4, 4,
			ESuperFAISSBankMetric::Dot, ESuperFAISSBankQuantization::Float32));
		int32 Index = INDEX_NONE;
		Scratch->Append({1.0f, 0.0f, 0.0f, 0.0f}, Index);
		TestTrue(TEXT("scratch bad dims"), IsToolError(
			USuperFAISSToolset::QueryScratchBank(Scratch->GetPathName(), {1.0f}, 3)));
	}

	// Id queries against an id-less bank are refused (import an id-less fixture).
	{
		FMCPFixture Fixture(TEXT("tool_idless"), 6, 4, /*bWithIds*/ false);
		// bAllowOverwrite: repeated suite runs in one editor session keep the
		// previous import's package in memory.
		const TSharedPtr<FJsonObject> R = ParseTool(USuperFAISSToolset::ImportBank(
			Fixture.JsonPath, ImportDestIdless, TEXT("Float32"), true));
		if (TestTrue(TEXT("idless import ok"), R.IsValid() && !R->HasField(TEXT("error"))))
		{
			TestTrue(TEXT("id on id-less bank refused"),
				IsToolError(USuperFAISSToolset::QueryBank(
					FString(ImportDestIdless) + TEXT(".ToolImportIdless"), TEXT("row0"),
					-1, {}, {}, {}, {}, {}, 3, false)));
		}
		const FString IdlessFile = FPackageName::LongPackageNameToFilename(
			ImportDestIdless, FPackageName::GetAssetPackageExtension());
		IFileManager::Get().Delete(*IdlessFile, false, true, true);
	}

	// ImportBank contract (W3): out-of-/Game destination, missing sidecar, bad
	// quantization — each refused with a tool-level error.
	TestTrue(TEXT("out of /Game"), IsToolError(USuperFAISSToolset::ImportBank(
		TEXT("C:/nope.wvbank.json"), TEXT("/SuperFAISSUnreal/Evil"), TEXT("Int8"), false)));
	TestTrue(TEXT("missing sidecar"), IsToolError(USuperFAISSToolset::ImportBank(
		TEXT("C:/definitely_missing.wvbank.json"), TEXT("/Game/X"), TEXT("Int8"), false)));
	{
		FMCPFixture Fixture(TEXT("tool_badquant"), 4, 4, true);
		TestTrue(TEXT("bad quantization"), IsToolError(USuperFAISSToolset::ImportBank(
			Fixture.JsonPath, TEXT("/Game/X"), TEXT("Int7"), false)));
	}

	// Prototype and axis failure modes.
	TestTrue(TEXT("empty prototype"), IsToolError(USuperFAISSToolset::QueryPrototype(
		DemoBankPath, {}, {}, FString(), 5)));
	TestTrue(TEXT("axis dims"), IsToolError(USuperFAISSToolset::ProjectAxis(
		DemoBankPath, {1.0f}, {2.0f}, 5)));

	return true;
}

// T-V2.3-U2 — the read-only report surface: DescribeScratchBank states the
// retention flag and, once a report was taken, the recall number WITH its
// generation stamp and stale mark — a stale report reads as stale through the
// surface, never silently current. MCP stays read-only (the R-M4 posture): the
// report is taken by game code; the tool only describes it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSMCPScratchRecallTest,
	"SuperFAISS.M.DescribeScratchRecall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSMCPScratchRecallTest::RunTest(const FString& Parameters)
{
	constexpr int32 Count = 32;
	constexpr int32 Dims = 8;

	// A retention bank with a measured report.
	USuperFAISSScratchBank* Audited = NewObject<USuperFAISSScratchBank>();
	TestTrue(TEXT("retention init"), Audited->Init(Count, Dims,
		ESuperFAISSBankMetric::Dot, ESuperFAISSBankQuantization::Int8,
		/*bRetainFloats*/ true));
	uint64 State = 0xA3A5ull;
	for (int32 R = 0; R < Count; ++R)
	{
		TArray<float> Row;
		Row.SetNumUninitialized(Dims);
		for (float& V : Row)
		{
			State ^= State >> 12;
			State ^= State << 25;
			State ^= State >> 27;
			V = static_cast<float>(static_cast<int64>((State * 0x2545F4914F6CDD1Dull) >> 24)) /
				static_cast<float>(1ll << 39);
		}
		int32 Index = INDEX_NONE;
		TestTrue(TEXT("append"), Audited->Append(Row, Index));
	}
	FSuperFAISSScratchRecallReport Report;
	TestTrue(TEXT("measured"), Audited->MeasureRecall(Report));

	const TSharedPtr<FJsonObject> D =
		ParseTool(USuperFAISSToolset::DescribeScratchBank(Audited->GetPathName()));
	if (TestTrue(TEXT("describe parses"), D.IsValid()))
	{
		TestTrue(TEXT("retention flag stated"), D->GetBoolField(TEXT("retainsFloats")));
		const TSharedPtr<FJsonObject>* Recall = nullptr;
		if (TestTrue(TEXT("recall report present"),
				D->TryGetObjectField(TEXT("recallReport"), Recall)))
		{
			TestEqual(TEXT("recall value"),
				static_cast<float>((*Recall)->GetNumberField(TEXT("recall"))),
				Report.Recall);
			TestEqual(TEXT("live rows"),
				(int32)(*Recall)->GetNumberField(TEXT("liveRows")), Report.LiveRows);
			TestEqual(TEXT("generation stamp"),
				(int64)(*Recall)->GetNumberField(TEXT("generation")), Report.Generation);
			TestFalse(TEXT("current at measurement"),
				(*Recall)->GetBoolField(TEXT("stale")));
			TestEqual(TEXT("informative stated"),
				(*Recall)->GetBoolField(TEXT("informative")), Report.bInformative);
		}
	}

	// A mutation after the report: the surface reads it as STALE.
	TestTrue(TEXT("mutating remove"), Audited->Remove(0));
	const TSharedPtr<FJsonObject> D2 =
		ParseTool(USuperFAISSToolset::DescribeScratchBank(Audited->GetPathName()));
	if (TestTrue(TEXT("describe re-parses"), D2.IsValid()))
	{
		const TSharedPtr<FJsonObject>* Recall = nullptr;
		if (TestTrue(TEXT("report still described"),
				D2->TryGetObjectField(TEXT("recallReport"), Recall)))
		{
			TestTrue(TEXT("stale reads stale"), (*Recall)->GetBoolField(TEXT("stale")));
		}
	}

	// A non-retention bank: flag off, no report object.
	USuperFAISSScratchBank* Plain = NewObject<USuperFAISSScratchBank>();
	TestTrue(TEXT("plain init"), Plain->Init(Count, Dims,
		ESuperFAISSBankMetric::Dot, ESuperFAISSBankQuantization::Int8));
	const TSharedPtr<FJsonObject> P =
		ParseTool(USuperFAISSToolset::DescribeScratchBank(Plain->GetPathName()));
	if (TestTrue(TEXT("plain describe parses"), P.IsValid()))
	{
		TestFalse(TEXT("retention off stated"), P->GetBoolField(TEXT("retainsFloats")));
		TestFalse(TEXT("no report object"), P->HasField(TEXT("recallReport")));
	}

	return true;
}

// T-V2.5-U3 (plan section 22, test design section 7): the V2.5 analytics MCP tools
// (ProjectionReport / SetToSetDistance / BankSpread) return the core helper's numbers
// for the same bank and arguments (parsed from the tool JSON), ProjectionReport extends
// the shipped ProjectAxis (same direction primitive, whole-set superset of its top-k, the
// N1 correction), and the tools are read-only (no bank mutation surface). Numeric match
// is to the JSON round-trip's precision — the MCP surface is a text protocol, not a
// bitwise one; the bitwise plugin<->core parity lives in SuperFAISS.A.AnalyticsSurface.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSMCPAnalyticsTest,
	"SuperFAISS.M.Analytics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

namespace
{
	const TCHAR* AnalyticsDest = TEXT("/Game/SuperFAISSMCPTests/AnalyticsInt8");
	const TCHAR* AnalyticsPath = TEXT("/Game/SuperFAISSMCPTests/AnalyticsInt8.AnalyticsInt8");

	void CleanupAnalyticsAsset()
	{
		const FString FileName = FPackageName::LongPackageNameToFilename(
			AnalyticsDest, FPackageName::GetAssetPackageExtension());
		IFileManager::Get().Delete(*FileName, false, true, true);
	}

	// Relative match to the JSON round-trip's precision (a text surface, not bitwise).
	bool NumMatches(double Json, float Subsystem)
	{
		const double S = static_cast<double>(Subsystem);
		return FMath::Abs(Json - S) <= 1.0e-4 * (1.0 + FMath::Abs(S));
	}
}

bool FSuperFAISSMCPAnalyticsTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	// Import a dedicated Int8 bank fixture so the analytics run on a cross-device bank.
	CleanupAnalyticsAsset();
	constexpr int32 Count = 32;
	constexpr int32 Dims = 8;
	FMCPFixture Fixture(TEXT("analytics_int8"), Count, Dims, /*bWithIds*/ true);
	const TSharedPtr<FJsonObject> Imported = ParseTool(USuperFAISSToolset::ImportBank(
		Fixture.JsonPath, AnalyticsDest, TEXT("Int8"), false));
	if (!TestTrue(TEXT("analytics fixture imported"),
			Imported.IsValid() && !Imported->HasField(TEXT("error"))))
	{
		CleanupAnalyticsAsset();
		return true;
	}

	USuperFAISSVectorBank* Bank = LoadObject<USuperFAISSVectorBank>(nullptr, AnalyticsPath);
	if (!TestNotNull(TEXT("analytics bank loads"), Bank))
	{
		CleanupAnalyticsAsset();
		return true;
	}

	TArray<int32> AllRows;
	AllRows.SetNumUninitialized(Bank->Count);
	for (int32 i = 0; i < Bank->Count; ++i)
	{
		AllRows[i] = i;
	}

	// --- BankSpread MCP == subsystem BankSpreadCrossDevice (mean and max) ---
	{
		const TSharedPtr<FJsonObject> R = ParseTool(USuperFAISSToolset::BankSpread(AnalyticsPath, {}));
		if (TestTrue(TEXT("BankSpread parses"), R.IsValid()) &&
			TestFalse(TEXT("BankSpread not error"), R->HasField(TEXT("error"))))
		{
			float RefMean = 0.0f;
			float RefMax = 0.0f;
			TestTrue(TEXT("subsystem spread mean"),
				Subsystem->BankSpreadCrossDevice(Bank, AllRows, ESuperFAISSReduce::Mean, RefMean));
			TestTrue(TEXT("subsystem spread max"),
				Subsystem->BankSpreadCrossDevice(Bank, AllRows, ESuperFAISSReduce::Max, RefMax));
			TestTrue(TEXT("MCP spreadMean matches core"),
				NumMatches(R->GetNumberField(TEXT("spreadMean")), RefMean));
			TestTrue(TEXT("MCP spreadMax matches core"),
				NumMatches(R->GetNumberField(TEXT("spreadMax")), RefMax));
		}
	}

	// --- SetToSetDistance MCP == subsystem (centroid + meanNN + maxNN) ---
	{
		const TArray<int32> SelA = {0, 1, 2, 3};
		const TArray<int32> SelB = {4, 5, 6, 7};
		const TSharedPtr<FJsonObject> R = ParseTool(USuperFAISSToolset::SetToSetDistance(
			AnalyticsPath, AnalyticsPath, SelA, SelB, TEXT("Cosine"), TEXT("all")));
		if (TestTrue(TEXT("SetToSetDistance parses"), R.IsValid()) &&
			TestFalse(TEXT("SetToSetDistance not error"), R->HasField(TEXT("error"))))
		{
			float RefCentroid = 0.0f;
			TestTrue(TEXT("subsystem centroid distance"),
				Subsystem->SetToSetDistanceCrossDevice(Bank, SelA, {}, Bank, SelB, {},
					ESuperFAISSBankMetric::Cosine, RefCentroid));
			TestTrue(TEXT("MCP centroidDistance matches core"),
				NumMatches(R->GetNumberField(TEXT("centroidDistance")), RefCentroid));

			float RefMean = 0.0f;
			float RefMax = 0.0f;
			TestTrue(TEXT("subsystem meanNN"),
				Subsystem->MeanNearestNeighborCrossDevice(Bank, Bank, RefMean));
			TestTrue(TEXT("subsystem maxNN"),
				Subsystem->MaxNearestNeighborCrossDevice(Bank, Bank, RefMax));
			TestTrue(TEXT("MCP meanNN matches core"),
				NumMatches(R->GetNumberField(TEXT("meanNN")), RefMean));
			TestTrue(TEXT("MCP maxNN matches core"),
				NumMatches(R->GetNumberField(TEXT("maxNN")), RefMax));
		}
	}

	// --- ProjectionReport MCP == subsystem, and the ProjectAxis superset (N1) ---
	{
		TArray<float> VecA;
		TArray<float> VecB;
		VecA.SetNumUninitialized(Dims);
		VecB.SetNumUninitialized(Dims);
		for (int32 J = 0; J < Dims; ++J)
		{
			VecA[J] = (J % 2 == 0) ? 1.0f : -0.5f;
			VecB[J] = (J % 3 == 0) ? -1.0f : 0.25f;
		}
		const TArray<int32> GroupA = {0, 1, 2, 3};
		const TSharedPtr<FJsonObject> R = ParseTool(USuperFAISSToolset::ProjectionReport(
			AnalyticsPath, VecA, VecB, GroupA));
		if (TestTrue(TEXT("ProjectionReport parses"), R.IsValid()) &&
			TestFalse(TEXT("ProjectionReport not error"), R->HasField(TEXT("error"))))
		{
			TArray<float> RefProjections;
			float RefSeparation = 0.0f;
			TestTrue(TEXT("subsystem projection"),
				Subsystem->ProjectionReport(Bank, VecA, VecB, GroupA, RefProjections, RefSeparation));
			TestTrue(TEXT("MCP separation matches core"),
				NumMatches(R->GetNumberField(TEXT("separation")), RefSeparation));

			const TArray<TSharedPtr<FJsonValue>>& Proj = R->GetArrayField(TEXT("projections"));
			TestEqual(TEXT("projection count"), Proj.Num(), RefProjections.Num());
			for (int32 i = 0; i < Proj.Num() && i < RefProjections.Num(); ++i)
			{
				TestTrue(TEXT("MCP projection[i] matches core"),
					NumMatches(Proj[i]->AsNumber(), RefProjections[i]));
			}

			// N1: ProjectAxis (top-k ranking on the same direction) is the base
			// ProjectionReport extends into a whole-set audit. Its top hits ARE the
			// highest-projected rows, so ProjectAxis's #1 must sit among the top few by
			// projection — the "superset of the top-k" relationship, robust to a last-bit
			// ranking difference between the two per-device paths.
			TArray<int32> ByProjection;
			ByProjection.SetNumUninitialized(RefProjections.Num());
			for (int32 i = 0; i < RefProjections.Num(); ++i)
			{
				ByProjection[i] = i;
			}
			ByProjection.Sort([&RefProjections](int32 A, int32 B) {
				return RefProjections[A] > RefProjections[B];
			});
			const int32 TopN = FMath::Min(3, ByProjection.Num());
			TSet<int32> TopByProjection;
			for (int32 i = 0; i < TopN; ++i)
			{
				TopByProjection.Add(ByProjection[i]);
			}

			const TSharedPtr<FJsonObject> Axis = ParseTool(USuperFAISSToolset::ProjectAxis(
				AnalyticsPath, VecA, VecB, 3));
			if (TestTrue(TEXT("ProjectAxis parses"), Axis.IsValid()) &&
				TestFalse(TEXT("ProjectAxis not error"), Axis->HasField(TEXT("error"))))
			{
				const TArray<TSharedPtr<FJsonValue>>& AxisHits = Axis->GetArrayField(TEXT("hits"));
				if (TestTrue(TEXT("ProjectAxis returned hits"), AxisHits.Num() > 0))
				{
					const int32 TopAxisIndex =
						static_cast<int32>(AxisHits[0]->AsObject()->GetNumberField(TEXT("index")));
					TestTrue(TEXT("ProjectAxis top-1 is among ProjectionReport's top-3 rows"),
						TopByProjection.Contains(TopAxisIndex));
				}
			}
		}
	}

	// --- Read-only posture (DEF): analytics tools mutate no bank; count is unchanged ---
	{
		const TSharedPtr<FJsonObject> D = ParseTool(USuperFAISSToolset::DescribeBank(AnalyticsPath));
		if (TestTrue(TEXT("post-analytics describe parses"), D.IsValid()))
		{
			TestEqual(TEXT("bank count unchanged after analytics reads"),
				static_cast<int32>(D->GetNumberField(TEXT("count"))), Count);
		}
		// An unknown mode is a defined rejection, not a silent all-run.
		TestTrue(TEXT("unknown set-to-set mode rejected"),
			IsToolError(USuperFAISSToolset::SetToSetDistance(
				AnalyticsPath, AnalyticsPath, {0}, {1}, TEXT("Cosine"), TEXT("bogus"))));
	}

	CleanupAnalyticsAsset();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
