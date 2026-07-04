// Cross-device exactness tests (V2.2, plan section 19 / T-058). Two layers:
//
// 1. The GOLDEN HASH battery — a faithful port of the core suite's T25 battery
//    over the committed fixture banks (vendored tests/xd_fixtures.h), asserting
//    the UE-COMPILED core reproduces the exact hash the core repo's CI pins on
//    Windows, Linux, and macOS-ARM. This is the cross-device claim's tripwire
//    inside the engine build environment, the way SimdMirrorEquality is the
//    FP-contraction tripwire: if UBT's compile environment ever perturbs the
//    integer kernels or the double epilogue, this fails on the spot.
//
// 2. The plugin surface — bCrossDeviceExact through QuerySync/Batch/Intersect/
//    Scratch and DecomposeHit, the f32 rejection, and batch == singles bitwise.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "SuperFAISSScratchBank.h"
#include "SuperFAISSSubsystem.h"
#include "SuperFAISSVectorBank.h"

#include "superfaiss/superfaiss.h"

#include "../../../ThirdParty/SuperFAISS/tests/xd_fixtures.h"

namespace
{
	using namespace superfaiss;

	// FNV-1a 64 over the battery's hits — byte-identical to the core suite's
	// hasher (count, then each hit's index and score bits, rank order).
	struct FBatteryHash
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

		void U32(uint32 V) { Bytes(&V, 4); }

		void Hits(const Hit* InHits, int32 InCount)
		{
			U32(static_cast<uint32>(InCount));
			for (int32 i = 0; i < InCount; ++i)
			{
				U32(static_cast<uint32>(InHits[i].index));
				uint32 Bits;
				FMemory::Memcpy(&Bits, &InHits[i].score, 4);
				U32(Bits);
			}
		}
	};

	// Fixture-backed bank view: rows point at the committed bytes; scales decode
	// from committed float bits — the same loader shape as the core suite.
	struct FFixBank
	{
		TArray<float> Scales;
		TArray<ChannelInfo> Channels;
		TArray<float> InvNorms;
		BankView View;

		FFixBank(const int8_t* Rows, const uint32_t* ScaleBits, int32 Count,
			int32 Dims, Metric InMetric, int32 ChannelCount = 0)
		{
			Scales.SetNumUninitialized(Count);
			for (int32 R = 0; R < Count; ++R)
			{
				FMemory::Memcpy(&Scales[R], &ScaleBits[R], 4);
			}
			View.rows = Rows;
			View.scales = Scales.GetData();
			View.count = Count;
			View.dims = Dims;
			View.paddedDims = PaddedDims(Dims, Quantization::Int8);
			View.quant = Quantization::Int8;
			View.metric = InMetric;
			if (ChannelCount > 0)
			{
				const int32 Len = Dims / ChannelCount;
				Channels.SetNumUninitialized(ChannelCount);
				for (int32 C = 0; C < ChannelCount; ++C)
				{
					Channels[C] = {C * Len, Len};
				}
				View.channels = Channels.GetData();
				View.channelCount = ChannelCount;
				InvNorms.SetNumUninitialized(Count * ChannelCount);
				ComputeChannelInverseNorms(View, InvNorms.GetData());
				View.channelInvNorms = InvNorms.GetData();
			}
		}
	};

	void LoadFixtureQuery(int32 Q, int32 Pd, float* Out)
	{
		FMemory::Memzero(Out, Pd * sizeof(float));
		const int32 N = Pd < xdfix::kQueryDims ? Pd : xdfix::kQueryDims;
		for (int32 i = 0; i < N; ++i)
		{
			FMemory::Memcpy(&Out[i], &xdfix::kQueryBits[Q * xdfix::kQueryDims + i], 4);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSCrossDeviceGoldenHashTest,
	"SuperFAISS.B.CrossDeviceGoldenHash",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSCrossDeviceGoldenHashTest::RunTest(const FString& Parameters)
{
	using namespace superfaiss;

	FBatteryHash Hash;
	Workspace Ws;

	FFixBank BankA(xdfix::kBankARows, xdfix::kBankAScaleBits, xdfix::kBankACount,
		xdfix::kBankADims, Metric::Dot);
	FFixBank BankB(xdfix::kBankBRows, xdfix::kBankBScaleBits, xdfix::kBankBCount,
		xdfix::kBankBDims, Metric::Cosine, xdfix::kBankBChannels);
	FFixBank BankC(xdfix::kBankCRows, xdfix::kBankCScaleBits, xdfix::kBankCCount,
		xdfix::kBankCDims, Metric::L2);
	FFixBank BankD(xdfix::kBankDRows, xdfix::kBankDScaleBits, xdfix::kBankDCount,
		xdfix::kBankDDims, Metric::Dot);
	FFixBank BankE(xdfix::kBankERows, xdfix::kBankEScaleBits, xdfix::kBankECount,
		xdfix::kBankEDims, Metric::L2);

	TArray<float, TAlignedHeapAllocator<16>> QueryBuf;
	QueryBuf.SetNumZeroed(64);
	Hit Hits[64 * 8];
	int32_t Counts[8];

	QueryParams XdParams;
	XdParams.exactness = Exactness::CrossDevice;

	bool bFloorHeld = true;
	auto Emit = [&](const Hit* H, int32 N)
	{
		for (int32 i = 0; i < N; ++i)
		{
			const float S = H[i].score;
			bFloorHeld &= (S == 0.0f || FMath::Abs(S) >= 1.1754943508222875e-38f);
		}
		Hash.Hits(H, N);
	};

	// --- Bank A: singles, exclusion, dense bias (subnormal values), sparse
	// bias, batch, intersect — the exact T25 sequence.
	{
		TArray<uint32> Exclude;
		Exclude.SetNumZeroed((BankA.View.count + 31) / 32);
		for (int32 R = 0; R < BankA.View.count; R += 3)
		{
			Exclude[R >> 5] |= 1u << (R & 31);
		}
		TArray<float> Dense;
		Dense.SetNumUninitialized(BankA.View.count);
		for (int32 R = 0; R < BankA.View.count; ++R)
		{
			const uint32 Bits = (R % 4 == 0) ? 0x00000007u
				: (R % 4 == 1) ? 0x80000123u
				: (R % 4 == 2) ? 0x3c23d70au
				: 0x00800000u;
			FMemory::Memcpy(&Dense[R], &Bits, 4);
		}
		const BiasPair Pairs[2] = {{5, 0.25f}, {40, -0.5f}};
		RowBias DenseBias;
		DenseBias.dense = Dense.GetData();
		RowBias PairBias;
		PairBias.pairs = Pairs;
		PairBias.pairCount = 2;

		for (int32 Q = 0; Q < xdfix::kQueryCount; ++Q)
		{
			LoadFixtureQuery(Q, BankA.View.paddedDims, QueryBuf.GetData());
			QueryParams P = XdParams;
			P.k = 10;
			TestTrue(TEXT("A.single"), Query(BankA.View, QueryBuf.GetData(), P, Ws,
				Hits, &Counts[0]) == Status::Ok);
			Emit(Hits, Counts[0]);

			P.excludeBits = Exclude.GetData();
			TestTrue(TEXT("A.excluded"), Query(BankA.View, QueryBuf.GetData(), P, Ws,
				Hits, &Counts[0]) == Status::Ok);
			Emit(Hits, Counts[0]);

			P.excludeBits = nullptr;
			P.bias = &DenseBias;
			TestTrue(TEXT("A.dense-bias"), Query(BankA.View, QueryBuf.GetData(), P, Ws,
				Hits, &Counts[0]) == Status::Ok);
			Emit(Hits, Counts[0]);

			P.bias = &PairBias;
			TestTrue(TEXT("A.sparse-bias"), Query(BankA.View, QueryBuf.GetData(), P, Ws,
				Hits, &Counts[0]) == Status::Ok);
			Emit(Hits, Counts[0]);
		}

		TArray<float, TAlignedHeapAllocator<16>> BatchBuf;
		BatchBuf.SetNumZeroed(5 * BankA.View.paddedDims);
		for (int32 Q = 0; Q < 5; ++Q)
		{
			LoadFixtureQuery(Q, BankA.View.paddedDims,
				BatchBuf.GetData() + Q * BankA.View.paddedDims);
		}
		QueryParams P = XdParams;
		P.k = 8;
		TestTrue(TEXT("A.batch"), QueryBatch(BankA.View, BatchBuf.GetData(), 5, P, Ws,
			Hits, Counts) == Status::Ok);
		for (int32 Q = 0; Q < 5; ++Q)
		{
			Emit(Hits + Q * P.k, Counts[Q]);
		}

		int32_t N = 0;
		TestTrue(TEXT("A.intersect"), QueryIntersect(BankA.View, BatchBuf.GetData(),
			3, P, Ws, Hits, &N) == Status::Ok);
		Emit(Hits, N);
	}

	// --- Bank B: whole-row, channel-matched segments, decomposition,
	// score-as-dot.
	{
		const QuerySegment Segs[3] = {
			{BankB.Channels[0].offset, BankB.Channels[0].length, 1.5f},
			{BankB.Channels[1].offset, BankB.Channels[1].length, 0.0f},
			{BankB.Channels[3].offset, BankB.Channels[3].length, -0.75f},
		};
		for (int32 Q = 0; Q < xdfix::kQueryCount; ++Q)
		{
			LoadFixtureQuery(Q, BankB.View.paddedDims, QueryBuf.GetData());
			QueryParams P = XdParams;
			P.k = 10;
			TestTrue(TEXT("B.single"), Query(BankB.View, QueryBuf.GetData(), P, Ws,
				Hits, &Counts[0]) == Status::Ok);
			Emit(Hits, Counts[0]);

			P.segments = Segs;
			P.segmentCount = 3;
			TestTrue(TEXT("B.segmented"), Query(BankB.View, QueryBuf.GetData(), P, Ws,
				Hits, &Counts[0]) == Status::Ok);
			Emit(Hits, Counts[0]);

			if (Counts[0] > 0)
			{
				TArray<int8, TAlignedHeapAllocator<16>> Q8;
				Q8.SetNumUninitialized(BankB.View.paddedDims);
				XdQuery Xd;
				QuantizeQueryXd(QueryBuf.GetData(), BankB.View.paddedDims,
					reinterpret_cast<int8_t*>(Q8.GetData()), &Xd.scale, &Xd.sqSum);
				Xd.q8 = reinterpret_cast<const int8_t*>(Q8.GetData());
				float Contributions[kMaxSegments];
				const float Total = DecomposeRowScoreXd(BankB.View, Xd,
					Hits[0].index, Segs, 3, Contributions);
				uint32 TotalBits, ScanBits;
				FMemory::Memcpy(&TotalBits, &Total, 4);
				FMemory::Memcpy(&ScanBits, &Hits[0].score, 4);
				TestEqual(TEXT("decompose total == scan score bitwise"),
					TotalBits, ScanBits);
				Hash.Bytes(Contributions, sizeof(float) * 3);
			}

			P.segments = nullptr;
			P.segmentCount = 0;
			P.scoreAs = ScoreAs::Dot;
			TestTrue(TEXT("B.score-as-dot"), Query(BankB.View, QueryBuf.GetData(), P,
				Ws, Hits, &Counts[0]) == Status::Ok);
			Emit(Hits, Counts[0]);
		}
	}

	// --- Bank C (L2): whole-row and segmented (the expanded-epilogue path).
	{
		const QuerySegment Segs[2] = {{0, 16, 1.0f}, {16, 16, 2.0f}};
		for (int32 Q = 0; Q < xdfix::kQueryCount; ++Q)
		{
			LoadFixtureQuery(Q, BankC.View.paddedDims, QueryBuf.GetData());
			QueryParams P = XdParams;
			P.k = 10;
			TestTrue(TEXT("C.single"), Query(BankC.View, QueryBuf.GetData(), P, Ws,
				Hits, &Counts[0]) == Status::Ok);
			Emit(Hits, Counts[0]);

			P.segments = Segs;
			P.segmentCount = 2;
			TestTrue(TEXT("C.segmented"), Query(BankC.View, QueryBuf.GetData(), P, Ws,
				Hits, &Counts[0]) == Status::Ok);
			Emit(Hits, Counts[0]);
		}
	}

	// --- Banks D and E (adversarial): full k so every row's score is pinned.
	{
		for (int32 Q = 0; Q < xdfix::kQueryCount; ++Q)
		{
			LoadFixtureQuery(Q, BankD.View.paddedDims, QueryBuf.GetData());
			QueryParams P = XdParams;
			P.k = BankD.View.count;
			TestTrue(TEXT("D.single"), Query(BankD.View, QueryBuf.GetData(), P, Ws,
				Hits, &Counts[0]) == Status::Ok);
			Emit(Hits, Counts[0]);
		}
		for (int32 Q = 0; Q < xdfix::kQueryCount; ++Q)
		{
			LoadFixtureQuery(Q, BankE.View.paddedDims, QueryBuf.GetData());
			QueryParams P = XdParams;
			P.k = BankE.View.count;
			TestTrue(TEXT("E.single"), Query(BankE.View, QueryBuf.GetData(), P, Ws,
				Hits, &Counts[0]) == Status::Ok);
			Emit(Hits, Counts[0]);
		}
	}

	TestTrue(TEXT("subnormal floor held on every battery hit"), bFloorHeld);
	TestEqual(TEXT("UE-compiled core reproduces the pinned cross-device hash"),
		Hash.H, xdfix::kGoldenXdHash);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSCrossDeviceSurfaceTest,
	"SuperFAISS.A.CrossDeviceSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSCrossDeviceSurfaceTest::RunTest(const FString& Parameters)
{
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}

	constexpr int32 Count = 200;
	constexpr int32 Dims = 32;
	TArray<float> Rows;
	Rows.SetNumUninitialized(Count * Dims);
	uint64 State = 0xD15Cull;
	auto NextFloat = [&State]()
	{
		State ^= State >> 12;
		State ^= State << 25;
		State ^= State >> 27;
		return static_cast<float>(static_cast<int64>((State * 0x2545F4914F6CDD1Dull) >> 24)) /
			static_cast<float>(1ll << 39);
	};
	for (float& V : Rows)
	{
		V = NextFloat();
	}
	TArray<float> Query;
	Query.SetNumUninitialized(Dims);
	for (float& V : Query)
	{
		V = NextFloat();
	}

	FString Error;
	USuperFAISSVectorBank* Int8Bank = NewObject<USuperFAISSVectorBank>();
	TestTrue(TEXT("int8 bank"), Int8Bank->InitFromSource(Rows, Count, Dims,
		ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Int8, {},
		TEXT("xd-test"), Error));
	USuperFAISSVectorBank* F32Bank = NewObject<USuperFAISSVectorBank>();
	TestTrue(TEXT("f32 bank"), F32Bank->InitFromSource(Rows, Count, Dims,
		ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Float32, {},
		TEXT("xd-test-f32"), Error));

	// The BP entry works on int8 and fails plainly on f32 (the int8-only law).
	TArray<FSuperFAISSHit> XdHits;
	TestTrue(TEXT("cross-device query on int8"),
		Subsystem->QuerySimilarCrossDevice(Int8Bank, Query, 10, XdHits));
	TestEqual(TEXT("k hits"), XdHits.Num(), 10);
	for (const FSuperFAISSHit& H : XdHits)
	{
		TestTrue(TEXT("floor law"), H.Score == 0.0f ||
			FMath::Abs(H.Score) >= 1.1754943508222875e-38f);
	}
	TArray<FSuperFAISSHit> F32Hits;
	TestFalse(TEXT("cross-device query on f32 is refused"),
		Subsystem->QuerySimilarCrossDevice(F32Bank, Query, 10, F32Hits));

	// Repeatability (same-process determinism floor of the cross-device claim).
	TArray<FSuperFAISSHit> Again;
	TestTrue(TEXT("repeat"), Subsystem->QuerySimilarCrossDevice(Int8Bank, Query, 10, Again));
	TestEqual(TEXT("repeat count"), Again.Num(), XdHits.Num());
	for (int32 i = 0; i < Again.Num(); ++i)
	{
		uint32 A, B;
		FMemory::Memcpy(&A, &Again[i].Score, 4);
		FMemory::Memcpy(&B, &XdHits[i].Score, 4);
		TestTrue(TEXT("repeat bitwise"), Again[i].Index == XdHits[i].Index && A == B);
	}

	// Batch == singles bitwise through the subsystem surface.
	FSuperFAISSQueryArgs Args;
	Args.K = 8;
	Args.bCrossDeviceExact = true;
	TArray<float> Batch;
	Batch.Append(Query);
	TArray<float> Query2;
	Query2.SetNumUninitialized(Dims);
	for (float& V : Query2)
	{
		V = NextFloat();
	}
	Batch.Append(Query2);
	TArray<FSuperFAISSHit> BatchHits;
	TArray<int32> BatchCounts;
	TestTrue(TEXT("batch"), Subsystem->QueryBatch(Int8Bank, Batch, 2, Args, BatchHits,
		BatchCounts));
	TArray<FSuperFAISSHit> Single2;
	TestTrue(TEXT("single 2"), Subsystem->QuerySync(Int8Bank, Query2, Args, Single2));
	TestEqual(TEXT("batch count"), BatchCounts[1], Single2.Num());
	for (int32 i = 0; i < Single2.Num(); ++i)
	{
		uint32 A, B;
		FMemory::Memcpy(&A, &BatchHits[Args.K + i].Score, 4);
		FMemory::Memcpy(&B, &Single2[i].Score, 4);
		TestTrue(TEXT("batch == single bitwise"),
			BatchHits[Args.K + i].Index == Single2[i].Index && A == B);
	}

	// Intersection honors the flag (QueryCount == 1 degenerates to the single).
	TArray<FSuperFAISSHit> IntersectHits;
	TestTrue(TEXT("intersect"), Subsystem->QueryIntersect(Int8Bank, Batch, 2, Args,
		IntersectHits));
	for (const FSuperFAISSHit& H : IntersectHits)
	{
		TestTrue(TEXT("intersect floor law"), H.Score == 0.0f ||
			FMath::Abs(H.Score) >= 1.1754943508222875e-38f);
	}

	// Scratch banks: cross-device queries run against snapshots.
	USuperFAISSScratchBank* Scratch = NewObject<USuperFAISSScratchBank>();
	TestTrue(TEXT("scratch init"), Scratch->Init(32, Dims,
		ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Int8));
	for (int32 R = 0; R < 16; ++R)
	{
		TArray<float> Row;
		Row.SetNumUninitialized(Dims);
		for (float& V : Row)
		{
			V = NextFloat();
		}
		int32 Index = -1;
		TestTrue(TEXT("append"), Scratch->Append(Row, Index));
	}
	TArray<FSuperFAISSHit> ScratchHits;
	TestTrue(TEXT("scratch cross-device"),
		Subsystem->QueryScratch(Scratch, Query, Args, ScratchHits));
	for (const FSuperFAISSHit& H : ScratchHits)
	{
		TestTrue(TEXT("scratch floor law"), H.Score == 0.0f ||
			FMath::Abs(H.Score) >= 1.1754943508222875e-38f);
	}

	// DecomposeHit in cross-device mode matches the cross-device scan bitwise.
	{
		const TArray<FName> ChannelNames = {TEXT("front"), TEXT("back")};
		const TArray<int32> Offsets = {0, 16};
		const TArray<int32> Lengths = {16, 16};
		USuperFAISSVectorBank* ChannelBank = NewObject<USuperFAISSVectorBank>();
		TestTrue(TEXT("channel bank"), ChannelBank->InitFromSource(Rows, Count, Dims,
			ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Int8, {},
			TEXT("xd-channel-test"), Error, ChannelNames, Offsets, Lengths));

		TArray<FSuperFAISSChannelWeight> Weights;
		Weights.Add({TEXT("front"), 1.0f});
		Weights.Add({TEXT("back"), 0.25f});
		FSuperFAISSQueryArgs ChannelArgs;
		ChannelArgs.K = 5;
		ChannelArgs.Channels = Weights;
		ChannelArgs.bCrossDeviceExact = true;
		TArray<FSuperFAISSHit> ChannelHits;
		TestTrue(TEXT("channel cross-device query"),
			Subsystem->QuerySync(ChannelBank, Query, ChannelArgs, ChannelHits));
		if (ChannelHits.Num() > 0)
		{
			TArray<float> Contributions;
			float Total = 0.0f;
			TestTrue(TEXT("decompose"), Subsystem->DecomposeHit(ChannelBank, Query,
				Weights, ChannelHits[0].Index, Contributions, Total, 0.0f,
				/*bCrossDeviceExact=*/true));
			uint32 A, B;
			FMemory::Memcpy(&A, &Total, 4);
			FMemory::Memcpy(&B, &ChannelHits[0].Score, 4);
			TestEqual(TEXT("decompose total == cross-device scan score bitwise"), A, B);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
