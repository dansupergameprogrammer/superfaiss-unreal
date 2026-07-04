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
#include "SuperFAISSToolset.h"
#include "SuperFAISSScratchBank.h"
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

#endif // WITH_DEV_AUTOMATION_TESTS
