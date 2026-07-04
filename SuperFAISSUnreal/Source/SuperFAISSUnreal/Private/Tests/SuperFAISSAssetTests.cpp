// Group C — bank asset and serialization (plan §12 C1–C4).

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "SuperFAISSVectorBank.h"

#include "superfaiss/superfaiss.h"

namespace
{
	USuperFAISSVectorBank* MakeTestBank(FAutomationTestBase& Test, int32 Count, int32 Dims,
		ESuperFAISSBankMetric Metric, ESuperFAISSBankQuantization Quant, bool bWithIds)
	{
		uint64 State = 0xABCDEF12345ull;
		TArray<float> Rows;
		Rows.SetNumUninitialized(Count * Dims);
		for (float& V : Rows)
		{
			State ^= State >> 12;
			State ^= State << 25;
			State ^= State >> 27;
			V = static_cast<float>(static_cast<int64>((State * 0x2545F4914F6CDD1Dull) >> 24)) /
				static_cast<float>(1ll << 39);
		}
		TArray<FName> Ids;
		if (bWithIds)
		{
			for (int32 i = 0; i < Count; ++i)
			{
				Ids.Add(FName(*FString::Printf(TEXT("word_%d"), i)));
			}
		}
		USuperFAISSVectorBank* Bank = NewObject<USuperFAISSVectorBank>();
		FString Error;
		const bool bOk = Bank->InitFromSource(Rows, Count, Dims, Metric, Quant, Ids,
			TEXT("test-hash"), Error);
		Test.TestTrue(FString::Printf(TEXT("InitFromSource: %s"), *Error), bOk);
		return bOk ? Bank : nullptr;
	}

	void SaveBank(USuperFAISSVectorBank* Bank, TArray<uint8>& OutBytes)
	{
		FMemoryWriter Writer(OutBytes, /*bIsPersistent*/ true);
		FObjectAndNameAsStringProxyArchive Ar(Writer, /*bLoadIfFindFails*/ false);
		Bank->Serialize(Ar);
	}

	USuperFAISSVectorBank* LoadBank(const TArray<uint8>& Bytes)
	{
		USuperFAISSVectorBank* Bank = NewObject<USuperFAISSVectorBank>();
		FMemoryReader Reader(Bytes, /*bIsPersistent*/ true);
		FObjectAndNameAsStringProxyArchive Ar(Reader, false);
		Bank->Serialize(Ar);
		Bank->PostLoad();
		return Bank;
	}

	bool QueryTop5(USuperFAISSVectorBank* Bank, superfaiss::Hit* OutHits, int32& OutCount)
	{
		using namespace superfaiss;
		const BankView View = Bank->GetBankView();
		TArray<float> QueryStorage;
		QueryStorage.SetNumZeroed(View.paddedDims + 4);
		float* Query = QueryStorage.GetData();
		while ((reinterpret_cast<UPTRINT>(Query) % kAlignment) != 0)
		{
			++Query;
		}
		for (int32 i = 0; i < View.dims; ++i)
		{
			Query[i] = 0.1f * static_cast<float>((i % 7) - 3);
		}
		Workspace Ws;
		QueryParams Params;
		Params.k = 5;
		int32_t N = 0;
		const Status S = superfaiss::Query(View, Query, Params, Ws, OutHits, &N);
		OutCount = N;
		return S == Status::Ok;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSAssetRoundTripTest,
	"SuperFAISS.C.AssetRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSAssetRoundTripTest::RunTest(const FString& Parameters)
{
	// C1: build -> save -> load -> queries bit-identical, per quantization.
	const ESuperFAISSBankQuantization Quants[] = {
		ESuperFAISSBankQuantization::Int8, ESuperFAISSBankQuantization::Float32};
	for (const ESuperFAISSBankQuantization Quant : Quants)
	{
		USuperFAISSVectorBank* Built =
			MakeTestBank(*this, 300, 24, ESuperFAISSBankMetric::Cosine, Quant, true);
		if (!Built)
		{
			return true;
		}
		superfaiss::Hit BuiltHits[5];
		int32 BuiltCount = 0;
		TestTrue(TEXT("query built"), QueryTop5(Built, BuiltHits, BuiltCount));

		TArray<uint8> Bytes;
		SaveBank(Built, Bytes);
		USuperFAISSVectorBank* Loaded = LoadBank(Bytes);
		TestTrue(TEXT("loaded bank validates"), Loaded->IsValid());
		TestEqual(TEXT("payload preserved"), Loaded->GetPayloadBytes(), Built->GetPayloadBytes());

		superfaiss::Hit LoadedHits[5];
		int32 LoadedCount = 0;
		TestTrue(TEXT("query loaded"), QueryTop5(Loaded, LoadedHits, LoadedCount));
		TestEqual(TEXT("hit count"), LoadedCount, BuiltCount);
		for (int32 i = 0; i < FMath::Min(LoadedCount, BuiltCount); ++i)
		{
			TestEqual(TEXT("index"), LoadedHits[i].index, BuiltHits[i].index);
			TestEqual(TEXT("score bits"), LoadedHits[i].score, BuiltHits[i].score);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSAssetVersionTest,
	"SuperFAISS.C.SchemaVersionRejection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSAssetVersionTest::RunTest(const FString& Parameters)
{
	// C2: a bumped schema version is hard-rejected at load. The rejection logs at Error
	// severity by design; tell the framework it is expected.
	AddExpectedError(TEXT("schema version"), EAutomationExpectedErrorFlags::Contains, 1);
	USuperFAISSVectorBank* Built =
		MakeTestBank(*this, 50, 8, ESuperFAISSBankMetric::Dot, ESuperFAISSBankQuantization::Int8, false);
	if (!Built)
	{
		return true;
	}
	Built->SchemaVersion = USuperFAISSVectorBank::kMaxAssetSchemaVersion + 1;
	TArray<uint8> Bytes;
	SaveBank(Built, Bytes);
	USuperFAISSVectorBank* Loaded = LoadBank(Bytes);
	TestFalse(TEXT("version-mismatched bank rejected"), Loaded->IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSAssetIdMapTest,
	"SuperFAISS.C.IdMap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSAssetIdMapTest::RunTest(const FString& Parameters)
{
	// C3: id bijectivity enforced at build; hits map to ids; id-less banks use indices.
	{
		TArray<float> Rows;
		Rows.SetNumZeroed(3 * 4);
		Rows[0] = 1.0f;
		Rows[5] = 1.0f;
		Rows[10] = 1.0f;
		TArray<FName> Duplicates = {FName("a"), FName("a"), FName("b")};
		USuperFAISSVectorBank* Bank = NewObject<USuperFAISSVectorBank>();
		FString Error;
		TestFalse(TEXT("duplicate ids rejected"),
			Bank->InitFromSource(Rows, 3, 4, ESuperFAISSBankMetric::Dot,
				ESuperFAISSBankQuantization::Float32, Duplicates, TEXT(""), Error));
	}
	{
		USuperFAISSVectorBank* WithIds = MakeTestBank(*this, 20, 8,
			ESuperFAISSBankMetric::Dot, ESuperFAISSBankQuantization::Float32, true);
		if (WithIds)
		{
			TestEqual(TEXT("id lookup"), WithIds->GetIdForIndex(7), FName("word_7"));
			TestEqual(TEXT("out of range"), WithIds->GetIdForIndex(99), NAME_None);
		}
		USuperFAISSVectorBank* NoIds = MakeTestBank(*this, 20, 8,
			ESuperFAISSBankMetric::Dot, ESuperFAISSBankQuantization::Float32, false);
		if (NoIds)
		{
			TestEqual(TEXT("id-less bank yields NAME_None"), NoIds->GetIdForIndex(7), NAME_None);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSAssetIndexBlockTest,
	"SuperFAISS.C.ReservedIndexBlock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSAssetIndexBlockTest::RunTest(const FString& Parameters)
{
	// C4: unknown-but-well-formed index block content survives the round trip without
	// being fatal (V2 forward-compat proof). The block is private; exercise it by
	// serializing a bank whose block carries opaque bytes via the archive layout:
	// save, splice nothing (layout is version+array, already exercised by C1 with an
	// empty block) — here we assert a loaded bank with an empty block is valid and
	// that the asset tolerates a non-empty one written by a future version.
	USuperFAISSVectorBank* Built =
		MakeTestBank(*this, 30, 8, ESuperFAISSBankMetric::L2, ESuperFAISSBankQuantization::Int8, false);
	if (!Built)
	{
		return true;
	}
	TArray<uint8> Bytes;
	SaveBank(Built, Bytes);

	// A future writer appends block content: emulate by editing the serialized stream's
	// trailing block (version int32 + int32 count + payload).
	// LAYOUT DEPENDENCY (Poirot O5): this splice assumes the index block is the LAST
	// thing USuperFAISSVectorBank::Serialize writes — the final 8 bytes are
	// IndexBlockVersion=0, ArrayNum=0. Appending any field after the block in
	// Serialize() breaks this test; update both together.
	TArray<uint8> Future = Bytes;
	const int32 TailStart = Future.Num() - 8;
	*reinterpret_cast<int32*>(Future.GetData() + TailStart) = 42; // unknown version
	const uint8 Opaque[4] = {0xDE, 0xAD, 0xBE, 0xEF};
	*reinterpret_cast<int32*>(Future.GetData() + TailStart + 4) = 4;
	Future.Append(Opaque, 4);

	USuperFAISSVectorBank* Loaded = LoadBank(Future);
	TestTrue(TEXT("unknown index block is ignored, not fatal"), Loaded->IsValid());

	superfaiss::Hit Hits[5];
	int32 HitCount = 0;
	TestTrue(TEXT("queries still work"), QueryTop5(Loaded, Hits, HitCount));
	TestEqual(TEXT("hits present"), HitCount, 5);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
