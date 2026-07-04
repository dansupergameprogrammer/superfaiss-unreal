// Group D — sidecar import, quantizer report, validation commandlet logic
// (plan §12 D1–D5). Fixtures are generated on the fly under the project's temp dir.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SuperFAISSBankImport.h"
#include "SuperFAISSVectorBank.h"

#include "superfaiss/superfaiss.h"

namespace
{
	struct FFixture
	{
		FString Dir;
		FString JsonPath;

		explicit FFixture(const FString& Name)
		{
			Dir = FPaths::ProjectIntermediateDir() / TEXT("SuperFAISSUnrealTests");
			IFileManager::Get().MakeDirectory(*Dir, true);
			JsonPath = Dir / (Name + TEXT(".wvbank.json"));
		}

		FString BinPath() const { return JsonPath.LeftChop(5) + TEXT(".bin"); }

		void WriteHeader(int32 Dims, int32 Count, const FString& Metric,
			const TArray<FString>* Ids = nullptr, int32 SchemaVersion = superfaiss::kSchemaVersion)
		{
			FString Json = FString::Printf(
				TEXT("{\n \"schemaVersion\": %d,\n \"dims\": %d,\n \"count\": %d,\n")
				TEXT(" \"metric\": \"%s\",\n \"dtype\": \"float32\""),
				SchemaVersion, Dims, Count, *Metric);
			if (Ids)
			{
				Json += TEXT(",\n \"ids\": [");
				for (int32 i = 0; i < Ids->Num(); ++i)
				{
					Json += FString::Printf(TEXT("%s\"%s\""), i ? TEXT(", ") : TEXT(""), *(*Ids)[i]);
				}
				Json += TEXT("]");
			}
			Json += TEXT("\n}\n");
			FFileHelper::SaveStringToFile(Json, *JsonPath);
		}

		void WriteRows(const TArray<float>& Rows)
		{
			const TArrayView<const uint8> Bytes(
				reinterpret_cast<const uint8*>(Rows.GetData()), Rows.Num() * sizeof(float));
			FFileHelper::SaveArrayToFile(Bytes, *BinPath());
		}
	};

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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSImportGoldenTest,
	"SuperFAISS.D.GoldenImport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSImportGoldenTest::RunTest(const FString& Parameters)
{
	// D1: a healthy sidecar imports; the bank validates, carries ids, and answers
	// a nearest-neighbor query with the expected row.
	FFixture Fx(TEXT("golden"));
	const int32 Dims = 12;
	const int32 Count = 64;
	TArray<float> Rows = SeededRows(Count, Dims, 7);
	// Make rows 10 and 30 near-identical so the golden query has a known answer.
	for (int32 i = 0; i < Dims; ++i)
	{
		Rows[30 * Dims + i] = Rows[10 * Dims + i] * 1.001f;
	}
	TArray<FString> Ids;
	for (int32 i = 0; i < Count; ++i)
	{
		Ids.Add(FString::Printf(TEXT("row_%d"), i));
	}
	Fx.WriteHeader(Dims, Count, TEXT("cosine"), &Ids);
	Fx.WriteRows(Rows);

	FString Error;
	USuperFAISSVectorBank* Bank = FSuperFAISSBankImport::Import(
		Fx.JsonPath, nullptr, TEXT("GoldenBank"), ESuperFAISSBankQuantization::Int8, Error);
	TestNotNull(FString::Printf(TEXT("import ok: %s"), *Error), Bank);
	if (!Bank)
	{
		return true;
	}
	TestTrue(TEXT("bank validates"), Bank->IsValid());
	TestEqual(TEXT("id map"), Bank->GetIdForIndex(30), FName("row_30"));

	// Query with row 10's vector (excluding itself): row 30 must be the top hit.
	using namespace superfaiss;
	const BankView View = Bank->GetBankView();
	TArray<float, TAlignedHeapAllocator<16>> QueryBuf;
	QueryBuf.SetNumZeroed(View.paddedDims);
	// Normalize row 10 the way the bake did.
	{
		TArray<float> Row10(Rows.GetData() + 10 * Dims, Dims);
		int32_t Bad = -1;
		NormalizeRows(Row10.GetData(), 1, Dims, &Bad);
		FMemory::Memcpy(QueryBuf.GetData(), Row10.GetData(), Dims * sizeof(float));
	}
	TArray<uint32> Exclude;
	Exclude.SetNumZeroed((Count + 31) / 32);
	Exclude[10 >> 5] |= 1u << (10 & 31);

	Workspace Ws;
	QueryParams Params;
	Params.k = 3;
	Params.excludeBits = Exclude.GetData();
	Hit Hits[3];
	int32_t N = 0;
	TestTrue(TEXT("query ok"),
		superfaiss::Query(View, QueryBuf.GetData(), Params, Ws, Hits, &N) == Status::Ok);
	TestEqual(TEXT("golden neighbor"), N > 0 ? Hits[0].index : -1, 30);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSImportRejectionTest,
	"SuperFAISS.D.RejectionMatrix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSImportRejectionTest::RunTest(const FString& Parameters)
{
	// D2: each malformed sidecar is rejected with its own diagnostic and no partial
	// asset. Every case returns null + non-empty error.
	const int32 Dims = 8;
	const int32 Count = 10;
	FString Error;

	auto ExpectReject = [&](FFixture& Fx, const TCHAR* Label)
	{
		Error.Reset();
		USuperFAISSVectorBank* Bank = FSuperFAISSBankImport::Import(
			Fx.JsonPath, nullptr, NAME_None, ESuperFAISSBankQuantization::Int8, Error);
		TestNull(Label, Bank);
		TestFalse(FString::Printf(TEXT("%s: has diagnostic"), Label), Error.IsEmpty());
	};

	// Truncated bin.
	{
		FFixture Fx(TEXT("truncated"));
		Fx.WriteHeader(Dims, Count, TEXT("dot"));
		TArray<float> Rows = SeededRows(Count, Dims, 11);
		Rows.SetNum(Rows.Num() - 3);
		Fx.WriteRows(Rows);
		ExpectReject(Fx, TEXT("truncated bin rejected"));
	}
	// Header/payload size mismatch (count lies).
	{
		FFixture Fx(TEXT("sizelie"));
		Fx.WriteHeader(Dims, Count + 5, TEXT("dot"));
		Fx.WriteRows(SeededRows(Count, Dims, 13));
		ExpectReject(Fx, TEXT("size mismatch rejected"));
	}
	// NaN row.
	{
		FFixture Fx(TEXT("nanrow"));
		Fx.WriteHeader(Dims, Count, TEXT("dot"));
		TArray<float> Rows = SeededRows(Count, Dims, 17);
		Rows[3 * Dims + 2] = NAN;
		Fx.WriteRows(Rows);
		ExpectReject(Fx, TEXT("NaN row rejected"));
	}
	// Duplicate ids.
	{
		FFixture Fx(TEXT("dupids"));
		TArray<FString> Ids;
		for (int32 i = 0; i < Count; ++i)
		{
			Ids.Add(TEXT("same"));
		}
		Fx.WriteHeader(Dims, Count, TEXT("dot"), &Ids);
		Fx.WriteRows(SeededRows(Count, Dims, 19));
		ExpectReject(Fx, TEXT("duplicate ids rejected"));
	}
	// Zero-norm row in a Cosine bank.
	{
		FFixture Fx(TEXT("zeronorm"));
		Fx.WriteHeader(Dims, Count, TEXT("cosine"));
		TArray<float> Rows = SeededRows(Count, Dims, 23);
		for (int32 i = 0; i < Dims; ++i)
		{
			Rows[5 * Dims + i] = 0.0f;
		}
		Fx.WriteRows(Rows);
		ExpectReject(Fx, TEXT("zero-norm cosine row rejected"));
	}
	// Wrong schema version.
	{
		FFixture Fx(TEXT("oldschema"));
		Fx.WriteHeader(Dims, Count, TEXT("dot"), nullptr, superfaiss::kSchemaVersion + 7);
		Fx.WriteRows(SeededRows(Count, Dims, 29));
		ExpectReject(Fx, TEXT("schema version rejected"));
	}
	// Unknown metric.
	{
		FFixture Fx(TEXT("badmetric"));
		Fx.WriteHeader(Dims, Count, TEXT("manhattan"));
		Fx.WriteRows(SeededRows(Count, Dims, 31));
		ExpectReject(Fx, TEXT("unknown metric rejected"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSImportRecallTest,
	"SuperFAISS.D.QuantizerRecall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSImportRecallTest::RunTest(const FString& Parameters)
{
	// D3: the recall report exists, is seeded, and clears its floor on realistic data.
	FFixture Fx(TEXT("recall"));
	const int32 Dims = 32;
	const int32 Count = 500;
	Fx.WriteHeader(Dims, Count, TEXT("cosine"));
	Fx.WriteRows(SeededRows(Count, Dims, 37));

	FString Error;
	USuperFAISSVectorBank* Bank = FSuperFAISSBankImport::Import(
		Fx.JsonPath, nullptr, TEXT("RecallBank"), ESuperFAISSBankQuantization::Int8, Error);
	TestNotNull(FString::Printf(TEXT("import ok: %s"), *Error), Bank);
	if (!Bank)
	{
		return true;
	}
	TestTrue(TEXT("recall seed recorded"), Bank->RecallSeed != 0);
	// Floor pinned at first bake per plan D3: int8 recall@10 on random data.
	TestTrue(FString::Printf(TEXT("recall@10 %.4f >= 0.90"), Bank->RecallAt10),
		Bank->RecallAt10 >= 0.90f);

	// Reproducibility: importing again yields the identical recall number.
	FString Error2;
	USuperFAISSVectorBank* Again = FSuperFAISSBankImport::Import(
		Fx.JsonPath, nullptr, TEXT("RecallBankAgain"), ESuperFAISSBankQuantization::Int8, Error2);
	if (TestNotNull(TEXT("second import"), Again))
	{
		TestEqual(TEXT("recall reproducible"), Again->RecallAt10, Bank->RecallAt10);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSImportHashTest,
	"SuperFAISS.D.SourceHashNoOp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSImportHashTest::RunTest(const FString& Parameters)
{
	// D4: unchanged sidecar pair -> unchanged hash (re-import no-op signal); changed
	// source -> changed hash.
	FFixture Fx(TEXT("hash"));
	Fx.WriteHeader(8, 20, TEXT("dot"));
	TArray<float> Rows = SeededRows(20, 8, 41);
	Fx.WriteRows(Rows);

	FString Error;
	FString HashA;
	FString HashB;
	TestTrue(TEXT("hash A"), FSuperFAISSBankImport::ComputeSourceHash(Fx.JsonPath, HashA, Error));
	TestTrue(TEXT("hash B"), FSuperFAISSBankImport::ComputeSourceHash(Fx.JsonPath, HashB, Error));
	TestEqual(TEXT("unchanged source, unchanged hash"), HashA, HashB);

	Rows[0] += 0.5f;
	Fx.WriteRows(Rows);
	FString HashC;
	TestTrue(TEXT("hash C"), FSuperFAISSBankImport::ComputeSourceHash(Fx.JsonPath, HashC, Error));
	TestNotEqual(TEXT("changed source, changed hash"), HashA, HashC);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSValidateSweepTest,
	"SuperFAISS.D.ValidateSweep",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperFAISSValidateSweepTest::RunTest(const FString& Parameters)
{
	// D5: the commandlet's sweep logic — healthy banks pass; a corrupted bank is
	// reported by path. (The commandlet itself is Main() + asset-registry glue over
	// this function.)
	FFixture Fx(TEXT("sweep"));
	Fx.WriteHeader(8, 20, TEXT("dot"));
	Fx.WriteRows(SeededRows(20, 8, 43));

	FString Error;
	USuperFAISSVectorBank* Healthy = FSuperFAISSBankImport::Import(
		Fx.JsonPath, nullptr, TEXT("SweepHealthy"), ESuperFAISSBankQuantization::Int8, Error);
	if (!TestNotNull(TEXT("healthy import"), Healthy))
	{
		return true;
	}

	TArray<FString> Invalid;
	FSuperFAISSBankImport::ValidateLoadedBanks(Invalid);
	TestFalse(TEXT("healthy bank not reported"),
		Invalid.ContainsByPredicate([&](const FString& P) { return P.Contains(TEXT("SweepHealthy")); }));

	// Corrupt one in memory: schema bump + revalidate via PostLoad path.
	AddExpectedError(TEXT("schema version"), EAutomationExpectedErrorFlags::Contains, 1);
	Healthy->SchemaVersion = superfaiss::kSchemaVersion + 1;
	Healthy->PostLoad();
	Invalid.Reset();
	const int32 InvalidCount = FSuperFAISSBankImport::ValidateLoadedBanks(Invalid);
	TestTrue(TEXT("corrupted bank reported"), InvalidCount >= 1 &&
		Invalid.ContainsByPredicate([&](const FString& P) { return P.Contains(TEXT("SweepHealthy")); }));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
