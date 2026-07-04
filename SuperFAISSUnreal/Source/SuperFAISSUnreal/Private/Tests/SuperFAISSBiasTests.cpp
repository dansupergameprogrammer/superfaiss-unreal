// Per-row bias tests (V2.1, plan section 18 / T-056): both forms through the
// subsystem, the N2 snapshot-alignment rejection on scratch banks, exactness of
// lift and eviction, the decomposition bias term, and the rejection matrix. The
// core's own composed-reference/batch/intersect proofs are T24.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "SuperFAISSScratchBank.h"
#include "SuperFAISSSubsystem.h"
#include "SuperFAISSVectorBank.h"

namespace
{
	TArray<float> BiasTestRows(int32 Count, int32 Dims, uint64 Seed)
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
	FSuperFAISSBiasTest,
	"SuperFAISS.A.RowBias",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSBiasTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	constexpr int32 Count = 300;
	constexpr int32 Dims = 32;
	const TArray<float> Rows = BiasTestRows(Count, Dims, 0xB1A5ull);
	const TArray<float> Query = BiasTestRows(1, Dims, 0x9E11ull);

	USuperFAISSVectorBank* Bank = NewObject<USuperFAISSVectorBank>();
	FString Error;
	TestTrue(TEXT("bank"), Bank->InitFromSource(Rows, Count, Dims,
		ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Int8, {},
		TEXT("bias-test"), Error));

	// Unbiased full ranking: the worst and best live rows.
	FSuperFAISSQueryArgs Full;
	Full.K = Count;
	TArray<FSuperFAISSHit> All;
	TestTrue(TEXT("full scan"), Subsystem->QuerySync(Bank, Query, Full, All));
	const int32 BottomRow = All.Last().Index;
	const int32 TopRow = All[0].Index;

	FSuperFAISSQueryArgs Args;
	Args.K = 10;

	// Sparse: lift the worst row to rank 1, evict the best; composed score is the
	// one-add contract against the unbiased score, bitwise.
	{
		Args.BiasPairs = {{BottomRow, 100.0f}, {TopRow, -100.0f}};
		TArray<FSuperFAISSHit> Hits;
		TestTrue(TEXT("sparse query"), Subsystem->QuerySync(Bank, Query, Args, Hits));
		TestEqual(TEXT("k hits"), Hits.Num(), 10);
		TestEqual(TEXT("lifted"), Hits[0].Index, BottomRow);
		TestEqual(TEXT("one-add contract"), Hits[0].Score, All.Last().Score + 100.0f);
		for (const FSuperFAISSHit& Hit : Hits)
		{
			TestTrue(TEXT("evicted"), Hit.Index != TopRow);
		}
		Args.BiasPairs.Reset();
	}

	// Dense: full-length view; wrong length is rejection (index-aligned or
	// rejected, never silently misaligned).
	{
		TArray<float> Dense;
		Dense.SetNumZeroed(Count);
		Dense[BottomRow] = 100.0f;
		Args.RowBias = Dense;
		TArray<FSuperFAISSHit> Hits;
		TestTrue(TEXT("dense query"), Subsystem->QuerySync(Bank, Query, Args, Hits));
		TestEqual(TEXT("dense lifted"), Hits[0].Index, BottomRow);

		Args.RowBias.SetNum(Count - 1);
		TestFalse(TEXT("dense length mismatch rejected"),
			Subsystem->QuerySync(Bank, Query, Args, Hits));
		Args.RowBias.Reset();
	}

	// Both forms set is rejection; non-finite is rejection.
	{
		TArray<FSuperFAISSHit> Hits;
		Args.RowBias.SetNumZeroed(Count);
		Args.BiasPairs = {{0, 1.0f}};
		TestFalse(TEXT("both forms rejected"),
			Subsystem->QuerySync(Bank, Query, Args, Hits));
		Args.RowBias.Reset();

		Args.BiasPairs = {{0, std::numeric_limits<float>::quiet_NaN()}};
		TestFalse(TEXT("non-finite pair rejected"),
			Subsystem->QuerySync(Bank, Query, Args, Hits));
		Args.BiasPairs.Reset();
	}

	// Decomposition bias term on a channel bank: contributions + RowBias == the
	// biased scan's own score for that hit, bitwise.
	{
		USuperFAISSVectorBank* ChannelBank = NewObject<USuperFAISSVectorBank>();
		const TArray<FName> Names = {TEXT("identity"), TEXT("appearance")};
		TestTrue(TEXT("channel bank"), ChannelBank->InitFromSource(Rows, Count, Dims,
			ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Int8, {},
			TEXT("bias-chan"), Error, Names, {0, Dims / 2}, {Dims / 2, Dims / 2}));

		FSuperFAISSQueryArgs ChanArgs;
		ChanArgs.K = 5;
		ChanArgs.Channels = {{TEXT("identity"), 1.0f}, {TEXT("appearance"), 0.5f}};
		TArray<FSuperFAISSHit> Unbiased;
		TestTrue(TEXT("channel query"),
			Subsystem->QuerySync(ChannelBank, Query, ChanArgs, Unbiased));
		const int32 Target = Unbiased[2].Index;
		const float PairBias = 0.75f;
		ChanArgs.BiasPairs = {{Target, PairBias}};
		TArray<FSuperFAISSHit> Biased;
		TestTrue(TEXT("channel+bias query"),
			Subsystem->QuerySync(ChannelBank, Query, ChanArgs, Biased));
		float ScanScore = 0.0f;
		bool bFound = false;
		for (const FSuperFAISSHit& Hit : Biased)
		{
			if (Hit.Index == Target)
			{
				ScanScore = Hit.Score;
				bFound = true;
			}
		}
		TestTrue(TEXT("biased row present"), bFound);
		TArray<float> Contributions;
		float Total = 0.0f;
		TestTrue(TEXT("decompose with bias"), Subsystem->DecomposeHit(ChannelBank,
			Query, ChanArgs.Channels, Target, Contributions, Total, PairBias));
		TestEqual(TEXT("bias term bitwise"), Total, ScanScore);
		TestEqual(TEXT("contributions + bias == total"),
			Contributions[0] + Contributions[1] + PairBias, Total);
	}

	// Scratch banks: bias is index-aligned to the snapshot (T-055 N2) - a stale
	// count is rejection; the pairs form composes with tombstone exclusion.
	{
		USuperFAISSScratchBank* Memory = NewObject<USuperFAISSScratchBank>();
		TestTrue(TEXT("scratch init"), Memory->Init(32, Dims,
			ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Float32));
		TArray<float> Row;
		Row.SetNumUninitialized(Dims);
		int32 Index = INDEX_NONE;
		for (int32 R = 0; R < 20; ++R)
		{
			FMemory::Memcpy(Row.GetData(), Rows.GetData() + R * Dims, Dims * sizeof(float));
			TestTrue(TEXT("scratch append"), Memory->Append(Row, Index));
		}

		FSuperFAISSQueryArgs ScratchArgs;
		ScratchArgs.K = 5;
		TArray<FSuperFAISSHit> Hits;
		TestTrue(TEXT("scratch unbiased"),
			Subsystem->QueryScratch(Memory, Query, ScratchArgs, Hits));

		// Dense sized for yesterday's snapshot (19) against today's 20: rejected.
		ScratchArgs.RowBias.SetNumZeroed(19);
		TestFalse(TEXT("stale dense rejected (N2)"),
			Subsystem->QueryScratch(Memory, Query, ScratchArgs, Hits));
		ScratchArgs.RowBias.SetNumZeroed(20);
		TestTrue(TEXT("aligned dense accepted"),
			Subsystem->QueryScratch(Memory, Query, ScratchArgs, Hits));
		ScratchArgs.RowBias.Reset();

		// A removed row stays excluded no matter the reward (mask beats arithmetic).
		FSuperFAISSQueryArgs Wide;
		Wide.K = 20;
		TArray<FSuperFAISSHit> AllScratch;
		TestTrue(TEXT("scratch full"),
			Subsystem->QueryScratch(Memory, Query, Wide, AllScratch));
		const int32 Victim = AllScratch[0].Index;
		TestTrue(TEXT("remove"), Memory->Remove(Victim));
		ScratchArgs.BiasPairs = {{Victim, 1000.0f}};
		TestTrue(TEXT("biased query after remove"),
			Subsystem->QueryScratch(Memory, Query, ScratchArgs, Hits));
		for (const FSuperFAISSHit& Hit : Hits)
		{
			TestTrue(TEXT("removed row stays out"), Hit.Index != Victim);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
