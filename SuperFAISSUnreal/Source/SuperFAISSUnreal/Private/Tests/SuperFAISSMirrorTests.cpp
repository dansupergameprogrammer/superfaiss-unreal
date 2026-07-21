// In-engine SIMD ≡ scalar-mirror bit-equality (core T11 equivalent; S1).
// This is the test that catches a compiler contracting the mirrors' float math —
// the failure mode that would otherwise surface months later as cross-machine
// replay divergence.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "superfaiss/superfaiss.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSMirrorEqualityTest,
	"SuperFAISS.B.SimdMirrorEquality",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSMirrorEqualityTest::RunTest(const FString& Parameters)
{
	using namespace superfaiss;

	uint64 State = 0x9E3779B97F4A7C15ull;
	auto NextFloat = [&State]() -> float
	{
		State ^= State >> 12;
		State ^= State << 25;
		State ^= State >> 27;
		return static_cast<float>(static_cast<int64>((State * 0x2545F4914F6CDD1Dull) >> 24)) /
			static_cast<float>(1ll << 39);
	};

	const int32 DimsSet[] = {8, 64, 256};
	for (const int32 Dims : DimsSet)
	{
		// float32 kernels.
		{
			const int32 Pd = PaddedDims(Dims, Quantization::Float32);
			TArray<float> Row;
			TArray<float> Query;
			Row.SetNumZeroed(Pd + 4);
			Query.SetNumZeroed(Pd + 4);
			float* RowP = Row.GetData();
			float* QueryP = Query.GetData();
			while ((reinterpret_cast<UPTRINT>(RowP) % kAlignment) != 0) { ++RowP; }
			while ((reinterpret_cast<UPTRINT>(QueryP) % kAlignment) != 0) { ++QueryP; }

			for (int32 Rep = 0; Rep < 25; ++Rep)
			{
				for (int32 i = 0; i < Dims; ++i)
				{
					RowP[i] = NextFloat();
					QueryP[i] = NextFloat();
				}
				TestEqual(TEXT("DotF32 == mirror"),
					detail::DotF32(RowP, QueryP, Pd), detail::DotF32Mirror(RowP, QueryP, Pd));
				TestEqual(TEXT("L2F32 == mirror"),
					detail::L2F32(RowP, QueryP, Pd), detail::L2F32Mirror(RowP, QueryP, Pd));
			}
		}
		// int8 kernels.
		{
			const int32 Pd = PaddedDims(Dims, Quantization::Int8);
			TArray<int8> Row;
			TArray<float> Query;
			Row.SetNumZeroed(Pd + 16);
			Query.SetNumZeroed(Pd + 4);
			int8* RowP = Row.GetData();
			float* QueryP = Query.GetData();
			while ((reinterpret_cast<UPTRINT>(RowP) % kAlignment) != 0) { ++RowP; }
			while ((reinterpret_cast<UPTRINT>(QueryP) % kAlignment) != 0) { ++QueryP; }

			for (int32 Rep = 0; Rep < 25; ++Rep)
			{
				for (int32 i = 0; i < Dims; ++i)
				{
					RowP[i] = static_cast<int8>(static_cast<int32>(NextFloat() * 127.0f));
					QueryP[i] = NextFloat();
				}
				const float Scale = 0.01f + 0.25f * (NextFloat() + 1.5f);
				TestEqual(TEXT("DotI8 == mirror"),
					detail::DotI8(RowP, Scale, QueryP, Pd),
					detail::DotI8Mirror(RowP, Scale, QueryP, Pd));
				TestEqual(TEXT("L2I8 == mirror"),
					detail::L2I8(RowP, Scale, QueryP, Pd),
					detail::L2I8Mirror(RowP, Scale, QueryP, Pd));
			}
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
