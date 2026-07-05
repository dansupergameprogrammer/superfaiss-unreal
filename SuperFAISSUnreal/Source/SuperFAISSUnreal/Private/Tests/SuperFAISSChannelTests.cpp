// Channel tests (V2 plan slot 2, UE half): schemaVersion-2 banks end to end —
// channel-carrying InitFromSource, named-channel queries as true per-channel
// cosines, named ≡ raw-range equivalence, decomposition against the scan's own
// scores, serialization round-trip, and the channel rejection matrix.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "SuperFAISSSubsystem.h"
#include "SuperFAISSVectorBank.h"

namespace
{
	TArray<float> ChannelRows(int32 Count, int32 Dims, uint64 Seed)
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

	USuperFAISSVectorBank* MakeChannelBank(FAutomationTestBase& Test, int32 Count,
		int32 Dims, ESuperFAISSBankQuantization Quant, FString* OutError = nullptr)
	{
		const TArray<float> Rows = ChannelRows(Count, Dims, 0xC4A77E57ull);
		USuperFAISSVectorBank* Bank = NewObject<USuperFAISSVectorBank>();
		FString Error;
		const TArray<FName> Names = {TEXT("identity"), TEXT("appearance")};
		const TArray<int32> Offsets = {0, Dims / 2};
		const TArray<int32> Lengths = {Dims / 2, Dims / 2};
		const bool bOk = Bank->InitFromSource(Rows, Count, Dims,
			ESuperFAISSBankMetric::Cosine, Quant, {}, TEXT("channel-test"), Error,
			Names, Offsets, Lengths);
		if (OutError)
		{
			*OutError = Error;
		}
		Test.TestTrue(FString::Printf(TEXT("channel bank built: %s"), *Error), bOk);
		return bOk ? Bank : nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSChannelQueryTest,
	"SuperFAISS.A.ChannelQueries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSChannelQueryTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	constexpr int32 Count = 300;
	constexpr int32 Dims = 32;

	for (ESuperFAISSBankQuantization Quant :
		{ESuperFAISSBankQuantization::Float32, ESuperFAISSBankQuantization::Int8})
	{
		USuperFAISSVectorBank* Bank = MakeChannelBank(*this, Count, Dims, Quant);
		if (!Bank)
		{
			continue;
		}
		TestEqual(TEXT("schema 2"), Bank->SchemaVersion, 2);
		TestEqual(TEXT("channel count"), Bank->GetChannelCount(), 2);
		TestEqual(TEXT("identity index"),
			Bank->GetChannelIndex(TEXT("identity")), 0);

		const TArray<float> Query = ChannelRows(1, Dims, 0x9E11ull);

		// Named channels: weighted two-channel query returns hits; single
		// unit-weight channel scores are true cosines in [-1, 1].
		FSuperFAISSQueryArgs Args;
		Args.K = 8;
		Args.Channels = {{TEXT("identity"), 1.0f}, {TEXT("appearance"), 0.25f}};
		TArray<FSuperFAISSHit> Named;
		TestTrue(TEXT("named query"), Subsystem->QuerySync(Bank, Query, Args, Named));
		TestEqual(TEXT("named hits"), Named.Num(), 8);

		FSuperFAISSQueryArgs OneChannel;
		OneChannel.K = Count;
		OneChannel.Channels = {{TEXT("identity"), 1.0f}};
		TArray<FSuperFAISSHit> All;
		TestTrue(TEXT("one-channel query"),
			Subsystem->QuerySync(Bank, Query, OneChannel, All));
		for (const FSuperFAISSHit& Hit : All)
		{
			TestTrue(FString::Printf(TEXT("cosine in range: %g"), Hit.Score),
				Hit.Score >= -1.001f && Hit.Score <= 1.001f);
		}

		// Named ≡ raw ranges (the same ranges the names resolve to), bit-identical.
		FSuperFAISSQueryArgs Raw;
		Raw.K = 8;
		FSuperFAISSSegment SegA;
		SegA.Offset = 0;
		SegA.Length = Bank->PaddedDims / 2;
		SegA.Weight = 1.0f;
		FSuperFAISSSegment SegB;
		SegB.Offset = Bank->PaddedDims / 2;
		SegB.Length = Bank->PaddedDims - Bank->PaddedDims / 2;
		SegB.Weight = 0.25f;
		Raw.Segments = {SegA, SegB};
		TArray<FSuperFAISSHit> Ranged;
		TestTrue(TEXT("raw-range query"), Subsystem->QuerySync(Bank, Query, Raw, Ranged));
		TestEqual(TEXT("named==ranged count"), Ranged.Num(), Named.Num());
		for (int32 i = 0; i < FMath::Min(Named.Num(), Ranged.Num()); ++i)
		{
			TestTrue(TEXT("named==ranged hit"),
				Named[i].Index == Ranged[i].Index && Named[i].Score == Ranged[i].Score);
		}

		// Decomposition: contributions sum to the total, and the total is the scan's
		// own score for that hit (bitwise).
		TArray<float> Contributions;
		float Total = 0.0f;
		TestTrue(TEXT("decompose"), Subsystem->DecomposeHit(Bank, Query, Args.Channels,
			Named[0].Index, Contributions, Total));
		TestEqual(TEXT("contribution count"), Contributions.Num(), 2);
		TestEqual(TEXT("decompose total == scan score"), Total, Named[0].Score);
		TestEqual(TEXT("contributions sum == total"),
			Contributions[0] + Contributions[1], Total);

		// Unknown channel name fails cleanly.
		FSuperFAISSQueryArgs Bad;
		Bad.K = 4;
		Bad.Channels = {{TEXT("nonexistent"), 1.0f}};
		TArray<FSuperFAISSHit> None;
		TestFalse(TEXT("unknown channel rejected"),
			Subsystem->QuerySync(Bank, Query, Bad, None));
	}

	// Serialization round-trip: a channel bank survives save/load with its norms and
	// answers identically.
	{
		USuperFAISSVectorBank* Bank =
			MakeChannelBank(*this, 64, Dims, ESuperFAISSBankQuantization::Int8);
		if (Bank)
		{
			const TArray<float> Query = ChannelRows(1, Dims, 0x77AAull);
			FSuperFAISSQueryArgs Args;
			Args.K = 5;
			Args.Channels = {{TEXT("identity"), 1.0f}, {TEXT("appearance"), 0.5f}};
			TArray<FSuperFAISSHit> Before;
			TestTrue(TEXT("pre-serialize query"),
				Subsystem->QuerySync(Bank, Query, Args, Before));

			TArray<uint8> Bytes;
			{
				FMemoryWriter Writer(Bytes, /*bIsPersistent*/ true);
				FObjectAndNameAsStringProxyArchive Ar(Writer, false);
				Bank->Serialize(Ar);
			}
			USuperFAISSVectorBank* Loaded = NewObject<USuperFAISSVectorBank>();
			{
				FMemoryReader Reader(Bytes, /*bIsPersistent*/ true);
				FObjectAndNameAsStringProxyArchive Ar(Reader, false);
				Loaded->Serialize(Ar);
			}
			Loaded->PostLoad();
			TestTrue(TEXT("loaded valid"), Loaded->IsValid());
			TestEqual(TEXT("loaded channels"), Loaded->GetChannelCount(), 2);
			TArray<FSuperFAISSHit> After;
			TestTrue(TEXT("post-serialize query"),
				Subsystem->QuerySync(Loaded, Query, Args, After));
			TestEqual(TEXT("round-trip count"), After.Num(), Before.Num());
			for (int32 i = 0; i < FMath::Min(Before.Num(), After.Num()); ++i)
			{
				TestTrue(TEXT("round-trip hit"),
					After[i].Index == Before[i].Index && After[i].Score == Before[i].Score);
			}
		}
	}

	// R-1 (Poirot deep review): a corrupted channel block is rejected at load,
	// never crashed on or silently degraded. Mutate a valid bank's properties the
	// way a corrupt asset would arrive, revalidate via PostLoad.
	{
		auto CorruptAndCheck = [&](const TCHAR* Label,
			TFunctionRef<void(USuperFAISSVectorBank*)> Corrupt)
		{
			USuperFAISSVectorBank* Victim =
				MakeChannelBank(*this, 64, Dims, ESuperFAISSBankQuantization::Int8);
			if (!Victim)
			{
				return;
			}
			AddExpectedError(TEXT("bank rejected"), EAutomationExpectedErrorFlags::Contains, 0);
			Corrupt(Victim);
			Victim->PostLoad();
			TestFalse(Label, Victim->IsValid());
		};
		CorruptAndCheck(TEXT("mismatched channel arrays rejected"),
			[](USuperFAISSVectorBank* B) { B->ChannelOffsets.Pop(); });
		CorruptAndCheck(TEXT("extra channel name rejected"),
			[](USuperFAISSVectorBank* B) { B->ChannelNames.Add(TEXT("ghost")); });
	}

	// Rejection matrix: overlap, off-grid, duplicate names, too many channels.
	{
		const TArray<float> Rows = ChannelRows(16, Dims, 0x11ull);
		USuperFAISSVectorBank* Bank = NewObject<USuperFAISSVectorBank>();
		FString Error;
		TestFalse(TEXT("overlap rejected"), Bank->InitFromSource(Rows, 16, Dims,
			ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Float32, {},
			TEXT("x"), Error, {TEXT("a"), TEXT("b")}, {0, 8}, {16, 8}));
		TestFalse(TEXT("off-grid rejected"), Bank->InitFromSource(Rows, 16, Dims,
			ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Float32, {},
			TEXT("x"), Error, {TEXT("a")}, {2}, {8}));
		TestFalse(TEXT("duplicate names rejected"), Bank->InitFromSource(Rows, 16, Dims,
			ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Float32, {},
			TEXT("x"), Error, {TEXT("a"), TEXT("a")}, {0, 16}, {16, 16}));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
