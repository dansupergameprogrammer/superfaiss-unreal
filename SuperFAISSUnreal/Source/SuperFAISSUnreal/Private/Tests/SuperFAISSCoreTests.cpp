// First UE automation tests: prove the harness end-to-end (plan S-V1) and exercise the
// vendored SuperFAISS core inside the engine environment. Mirrors the core harness's
// known-geometry and tie-break cases (plan A6, B3 seeds); the full A/B groups land with
// the subsystem layers.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "superfaiss/superfaiss.h"

namespace
{
	struct FAlignedQuery
	{
		alignas(16) float Data[16] = {};
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSKnownGeometryTest,
	"SuperFAISS.A.KnownGeometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSKnownGeometryTest::RunTest(const FString& Parameters)
{
	using namespace superfaiss;

	// dims=4 unit axes plus a diagonal; hand-computed neighbor order (plan A6).
	const float RowData[5][4] = {
		{1, 0, 0, 0},
		{0, 1, 0, 0},
		{0, 0, 1, 0},
		{0, 0, 0, 1},
		{0.5f, 0.5f, 0.5f, 0.5f},
	};
	alignas(16) float Payload[5 * 4];
	PadRowsFloat32(&RowData[0][0], 5, 4, 4, Payload);

	BankView Bank;
	Bank.rows = Payload;
	Bank.count = 5;
	Bank.dims = 4;
	Bank.paddedDims = 4;
	Bank.quant = Quantization::Float32;
	Bank.metric = Metric::Dot;
	TestEqual(TEXT("bank validates"), static_cast<int>(ValidateBank(Bank)), static_cast<int>(Status::Ok));

	FAlignedQuery Query16;
	Query16.Data[0] = 0.9f;
	Query16.Data[1] = 0.1f;

	Workspace Ws;
	Hit Hits[3];
	int32_t HitCount = 0;
	QueryParams Params;
	Params.k = 3;

	const Status Result = superfaiss::Query(Bank, Query16.Data, Params, Ws, Hits, &HitCount);
	TestEqual(TEXT("query status"), static_cast<int>(Result), static_cast<int>(Status::Ok));
	TestEqual(TEXT("hit count"), HitCount, 3);
	if (HitCount == 3)
	{
		// dot scores: row0 = 0.9, row4 = 0.5, row1 = 0.1.
		TestEqual(TEXT("best"), Hits[0].index, 0);
		TestEqual(TEXT("second"), Hits[1].index, 4);
		TestEqual(TEXT("third"), Hits[2].index, 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSTieBreakTest,
	"SuperFAISS.B.TieBreakStability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSTieBreakTest::RunTest(const FString& Parameters)
{
	using namespace superfaiss;

	// Four identical rows, two worse rows: ties must return ascending indices (plan B3).
	alignas(16) float Payload[6 * 4];
	const float RowValue[4] = {0.25f, -0.5f, 0.75f, 0.125f};
	for (int32 Row = 0; Row < 4; ++Row)
	{
		FMemory::Memcpy(&Payload[Row * 4], RowValue, sizeof(RowValue));
	}
	for (int32 Row = 4; Row < 6; ++Row)
	{
		for (int32 Lane = 0; Lane < 4; ++Lane)
		{
			Payload[Row * 4 + Lane] = -1.0f;
		}
	}

	BankView Bank;
	Bank.rows = Payload;
	Bank.count = 6;
	Bank.dims = 4;
	Bank.paddedDims = 4;
	Bank.quant = Quantization::Float32;
	Bank.metric = Metric::Dot;

	FAlignedQuery Query16;
	FMemory::Memcpy(Query16.Data, RowValue, sizeof(RowValue));

	Workspace Ws;
	Hit Hits[4];
	int32_t HitCount = 0;
	QueryParams Params;
	Params.k = 4;

	const Status Result = superfaiss::Query(Bank, Query16.Data, Params, Ws, Hits, &HitCount);
	TestEqual(TEXT("query status"), static_cast<int>(Result), static_cast<int>(Status::Ok));
	TestEqual(TEXT("hit count"), HitCount, 4);
	if (HitCount == 4)
	{
		for (int32 i = 0; i < 4; ++i)
		{
			TestEqual(TEXT("tie order"), Hits[i].index, i);
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
