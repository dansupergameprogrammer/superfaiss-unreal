// Scratch-bank channel SURFACE completion tests (T-099 slot 5, the BP-facing surface
// left after the engine-level channel query landed). Two surfaces:
//   (1) USuperFAISSSubsystem::QuerySimilarChannelsScratch — the Blueprint sibling of the
//       baked QuerySimilarChannels, asserted bit-identical to the C++ QueryScratch it
//       wraps (a named-channel query on a scratch bank);
//   (2) USuperFAISSScratchBank::MeasureRecallPerChannel — the scratch closure of the
//       core v3.0 MeasureScratchRecallPerChannel (D-V3-7), one recall report per channel,
//       plus its defined rejections (non-retention bank, channel-less bank).
//
// Mirrors SuperFAISSScratchChannelTests.cpp's fixtures (a Cosine channel scratch bank,
// the deterministic row generator) so the two files read as one surface.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "SuperFAISSScratchBank.h"
#include "SuperFAISSSubsystem.h"

namespace
{
	// Deterministic xorshift float rows — the same generator the sibling scratch/channel
	// test files use, reproduced per-TU so this file's fixtures are self-contained.
	TArray<float> SurfaceRows(int32 Count, int32 Dims, uint64 Seed)
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

// -----------------------------------------------------------------------------------
// Cell 1 — QuerySimilarChannelsScratch (BP) is bit-identical to the C++ QueryScratch it
// wraps: same channels, same K, same query -> same hit indices AND same scores. The BP
// wrapper adds nothing to the ranking; it only assembles the args a C++ caller assembles
// by hand. Both Float32 and Int8, over a two-channel Cosine bank.
// -----------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSScratchChannelBPWrapperTest,
	"SuperFAISS.A.ScratchChannelBPWrapper",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSScratchChannelBPWrapperTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	constexpr int32 Count = 48;
	constexpr int32 Dims = 32;
	const TArray<FName> Names = {TEXT("identity"), TEXT("appearance")};
	const TArray<int32> Offsets = {0, 16};
	const TArray<int32> Lengths = {16, 16};

	for (ESuperFAISSBankQuantization Quant :
		{ESuperFAISSBankQuantization::Float32, ESuperFAISSBankQuantization::Int8})
	{
		USuperFAISSScratchBank* Bank = NewObject<USuperFAISSScratchBank>();
		TestTrue(TEXT("InitWithChannels"),
			Bank->InitWithChannels(Count, Dims, ESuperFAISSBankMetric::Cosine, Quant,
				Names, Offsets, Lengths));

		const TArray<float> Rows = SurfaceRows(Count, Dims, 0xB19A7E5Full);
		TArray<float> Row;
		Row.SetNumUninitialized(Dims);
		int32 Index = INDEX_NONE;
		for (int32 R = 0; R < Count; ++R)
		{
			FMemory::Memcpy(Row.GetData(), Rows.GetData() + R * Dims, Dims * sizeof(float));
			TestTrue(TEXT("append"), Bank->Append(Row, Index));
		}

		const TArray<float> Query = SurfaceRows(1, Dims, 0x0DDBA11ull);
		const TArray<FSuperFAISSChannelWeight> Channels =
			{{TEXT("identity"), 1.0f}, {TEXT("appearance"), 0.35f}};

		// The BP wrapper.
		TArray<FSuperFAISSHit> WrapperHits;
		TestTrue(TEXT("QuerySimilarChannelsScratch succeeds"),
			Subsystem->QuerySimilarChannelsScratch(Bank, Query, Channels, 8, WrapperHits));

		// The hand-assembled C++ equivalent it must equal exactly.
		FSuperFAISSQueryArgs Args;
		Args.K = 8;
		Args.Channels = Channels;
		TArray<FSuperFAISSHit> DirectHits;
		TestTrue(TEXT("direct QueryScratch succeeds"),
			Subsystem->QueryScratch(Bank, Query, Args, DirectHits));

		TestEqual(TEXT("wrapper hit count == direct"), WrapperHits.Num(), DirectHits.Num());
		for (int32 i = 0; i < FMath::Min(WrapperHits.Num(), DirectHits.Num()); ++i)
		{
			TestTrue(FString::Printf(TEXT("wrapper==direct hit %d"), i),
				WrapperHits[i].Index == DirectHits[i].Index &&
					WrapperHits[i].Score == DirectHits[i].Score);
		}
	}

	return true;
}

// -----------------------------------------------------------------------------------
// Cell 2 — MeasureRecallPerChannel returns one report per channel on a retention-enabled
// Cosine channel bank, each report over the appended live rows; and rejects (empty
// output) on a non-retention bank and on a channel-less bank. The per-channel closure of
// the core v3.0 MeasureScratchRecallPerChannel (D-V3-7).
// -----------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSScratchPerChannelRecallTest,
	"SuperFAISS.A.ScratchPerChannelRecall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSScratchPerChannelRecallTest::RunTest(const FString& Parameters)
{
	constexpr int32 Count = 160;
	constexpr int32 Dims = 32;
	const TArray<FName> Names = {TEXT("identity"), TEXT("appearance")};
	const TArray<int32> Offsets = {0, 16};
	const TArray<int32> Lengths = {16, 16};

	auto FillBank = [&](USuperFAISSScratchBank* Bank, int32 N)
	{
		const TArray<float> Rows = SurfaceRows(N, Dims, 0x4EC0110Cull);
		TArray<float> Row;
		Row.SetNumUninitialized(Dims);
		int32 Index = INDEX_NONE;
		for (int32 R = 0; R < N; ++R)
		{
			FMemory::Memcpy(Row.GetData(), Rows.GetData() + R * Dims, Dims * sizeof(float));
			Bank->Append(Row, Index);
		}
	};

	// Retention-enabled Cosine channel bank: per-channel recall is defined.
	{
		USuperFAISSScratchBank* Bank = NewObject<USuperFAISSScratchBank>();
		TestTrue(TEXT("InitWithChannels (retain floats)"),
			Bank->InitWithChannels(Count, Dims, ESuperFAISSBankMetric::Cosine,
				ESuperFAISSBankQuantization::Int8, Names, Offsets, Lengths,
				/*bRetainFloats=*/true));
		FillBank(Bank, Count);

		TArray<FSuperFAISSScratchRecallReport> Reports;
		TestTrue(TEXT("MeasureRecallPerChannel succeeds"),
			Bank->MeasureRecallPerChannel(Reports));
		TestEqual(TEXT("one report per channel"), Reports.Num(), Names.Num());
		for (int32 C = 0; C < Reports.Num(); ++C)
		{
			TestEqual(FString::Printf(TEXT("channel %d live rows == count"), C),
				Reports[C].LiveRows, Count);
			TestTrue(FString::Printf(TEXT("channel %d k > 0"), C), Reports[C].K > 0);
			TestTrue(FString::Printf(TEXT("channel %d recall in [0,1]"), C),
				Reports[C].Recall >= 0.0f && Reports[C].Recall <= 1.0001f);
			TestTrue(FString::Printf(TEXT("channel %d informative at 160 rows"), C),
				Reports[C].bInformative);
			TestEqual(FString::Printf(TEXT("channel %d generation == bank"), C),
				static_cast<uint64>(Reports[C].Generation), Bank->Core().Generation());
		}
	}

	// Non-retention bank: no float reference -> defined rejection, not a guessed number.
	{
		USuperFAISSScratchBank* Bank = NewObject<USuperFAISSScratchBank>();
		TestTrue(TEXT("InitWithChannels (no retention)"),
			Bank->InitWithChannels(Count, Dims, ESuperFAISSBankMetric::Cosine,
				ESuperFAISSBankQuantization::Int8, Names, Offsets, Lengths,
				/*bRetainFloats=*/false));
		FillBank(Bank, Count);
		TArray<FSuperFAISSScratchRecallReport> Reports;
		TestFalse(TEXT("per-channel recall rejected on non-retention bank"),
			Bank->MeasureRecallPerChannel(Reports));
		TestEqual(TEXT("no reports on rejection"), Reports.Num(), 0);
	}

	// Channel-less bank: nothing to scope per channel -> defined rejection (MeasureRecall
	// is that bank's surface).
	{
		USuperFAISSScratchBank* Bank = NewObject<USuperFAISSScratchBank>();
		TestTrue(TEXT("Init (channel-less, retain floats)"),
			Bank->Init(Count, Dims, ESuperFAISSBankMetric::Cosine,
				ESuperFAISSBankQuantization::Int8, /*bRetainFloats=*/true));
		FillBank(Bank, Count);
		TArray<FSuperFAISSScratchRecallReport> Reports;
		TestFalse(TEXT("per-channel recall rejected on channel-less bank"),
			Bank->MeasureRecallPerChannel(Reports));
	}

	return true;
}

// -----------------------------------------------------------------------------------
// Cell 3 — save/load restores the host channel vocabulary (S2). The core archive
// carries the channel ranges but not the host FName names; the plugin appends a name
// frame so a saved channel-carrying scratch bank round-trips into a FRESH bank with its
// named-channel queries intact. A channel-less bank round-trips unchanged (the frame is a
// zero-count trailer). Reject-over-degrade: a truncated blob leaves the target unchanged.
// -----------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSScratchChannelSaveLoadTest,
	"SuperFAISS.A.ScratchChannelSaveLoad",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSScratchChannelSaveLoadTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	constexpr int32 Count = 48;
	constexpr int32 Dims = 32;
	const TArray<FName> Names = {TEXT("identity"), TEXT("appearance")};
	const TArray<int32> Offsets = {0, 16};
	const TArray<int32> Lengths = {16, 16};
	const TArray<float> Rows = SurfaceRows(Count, Dims, 0x5A7E10ADull);
	const TArray<float> Query = SurfaceRows(1, Dims, 0xC4A9E12Bull);
	const TArray<FSuperFAISSChannelWeight> Channels =
		{{TEXT("identity"), 1.0f}, {TEXT("appearance"), 0.4f}};

	// Source channel bank + a reference named-channel ranking.
	USuperFAISSScratchBank* Source = NewObject<USuperFAISSScratchBank>();
	TestTrue(TEXT("source InitWithChannels"),
		Source->InitWithChannels(Count, Dims, ESuperFAISSBankMetric::Cosine,
			ESuperFAISSBankQuantization::Float32, Names, Offsets, Lengths));
	{
		TArray<float> Row;
		Row.SetNumUninitialized(Dims);
		int32 Index = INDEX_NONE;
		for (int32 R = 0; R < Count; ++R)
		{
			FMemory::Memcpy(Row.GetData(), Rows.GetData() + R * Dims, Dims * sizeof(float));
			Source->Append(Row, Index);
		}
	}
	TArray<FSuperFAISSHit> Reference;
	TestTrue(TEXT("source named query"),
		Subsystem->QuerySimilarChannelsScratch(Source, Query, Channels, 8, Reference));

	TArray<uint8> Blob;
	TestTrue(TEXT("save"), Source->SaveToBytes(Blob));

	// Load into a FRESH, channel-less bank: the vocabulary must come back from the blob.
	USuperFAISSScratchBank* Loaded = NewObject<USuperFAISSScratchBank>();
	TestTrue(TEXT("load into fresh bank"), Loaded->LoadFromBytes(Blob));
	TestEqual(TEXT("channel count restored"), Loaded->GetChannelCount(), Names.Num());
	TestEqual(TEXT("identity index restored"), Loaded->GetChannelIndex(TEXT("identity")), 0);
	TestEqual(TEXT("appearance index restored"), Loaded->GetChannelIndex(TEXT("appearance")), 1);

	TArray<FSuperFAISSHit> Restored;
	TestTrue(TEXT("named query on loaded bank"),
		Subsystem->QuerySimilarChannelsScratch(Loaded, Query, Channels, 8, Restored));
	TestEqual(TEXT("loaded==source hit count"), Restored.Num(), Reference.Num());
	for (int32 i = 0; i < FMath::Min(Restored.Num(), Reference.Num()); ++i)
	{
		TestTrue(FString::Printf(TEXT("loaded==source hit %d"), i),
			Restored[i].Index == Reference[i].Index && Restored[i].Score == Reference[i].Score);
	}

	// A channel-less bank round-trips unchanged (the frame is a zero-count trailer).
	{
		USuperFAISSScratchBank* Plain = NewObject<USuperFAISSScratchBank>();
		TestTrue(TEXT("plain init"), Plain->Init(Count, Dims, ESuperFAISSBankMetric::Cosine,
			ESuperFAISSBankQuantization::Float32));
		TArray<float> Row;
		Row.SetNumUninitialized(Dims);
		int32 Index = INDEX_NONE;
		for (int32 R = 0; R < Count; ++R)
		{
			FMemory::Memcpy(Row.GetData(), Rows.GetData() + R * Dims, Dims * sizeof(float));
			Plain->Append(Row, Index);
		}
		TArray<uint8> PlainBlob;
		TestTrue(TEXT("plain save"), Plain->SaveToBytes(PlainBlob));
		USuperFAISSScratchBank* PlainLoaded = NewObject<USuperFAISSScratchBank>();
		TestTrue(TEXT("plain load"), PlainLoaded->LoadFromBytes(PlainBlob));
		TestEqual(TEXT("plain load has no channels"), PlainLoaded->GetChannelCount(), 0);
		TestEqual(TEXT("plain load count"), PlainLoaded->GetCount(), Count);
	}

	// Reject-over-degrade: a truncated blob leaves the target bank unchanged.
	{
		USuperFAISSScratchBank* Target = NewObject<USuperFAISSScratchBank>();
		TestTrue(TEXT("target InitWithChannels"),
			Target->InitWithChannels(Count, Dims, ESuperFAISSBankMetric::Cosine,
				ESuperFAISSBankQuantization::Float32, Names, Offsets, Lengths));
		TArray<uint8> Truncated = Blob;
		Truncated.SetNum(Blob.Num() / 2);
		TestFalse(TEXT("truncated load rejected"), Target->LoadFromBytes(Truncated));
		TestEqual(TEXT("target channels intact after rejected load"),
			Target->GetChannelIndex(TEXT("identity")), 0);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
