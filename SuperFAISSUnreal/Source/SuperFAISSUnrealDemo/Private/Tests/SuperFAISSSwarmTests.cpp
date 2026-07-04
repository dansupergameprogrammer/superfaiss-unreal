// F2 — Station 2 stability (plan §12): the swarm runs 10 simulated seconds; every
// entity holds a valid archetype assignment; the batch query time stays under its
// calibrated ceiling. Drives USuperFAISSSwarmSubsystem::Step — the same path the
// world tick drives — against real Mass entities in the automation world.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "SuperFAISSSwarmSubsystem.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperFAISSSwarmStabilityTest,
	"SuperFAISS.F.SwarmStability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
		EAutomationTestFlags::ProductFilter)

bool FSuperFAISSSwarmStabilityTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::Editor ||
			Context.WorldType == EWorldType::Game)
		{
			World = Context.World();
			break;
		}
	}
	if (!TestNotNull(TEXT("world"), World))
	{
		return true;
	}
	USuperFAISSSwarmSubsystem* Swarm = World->GetSubsystem<USuperFAISSSwarmSubsystem>();
	if (!TestNotNull(TEXT("swarm subsystem"), Swarm))
	{
		return true;
	}

	constexpr int32 EntityCount = 2000;
	constexpr int32 ArchetypeCount = 64;
	constexpr int32 Dims = 16;
	if (!TestTrue(TEXT("swarm started"),
		Swarm->StartSwarm(EntityCount, ArchetypeCount, Dims, /*Seed*/ 7)))
	{
		return true;
	}
	TestEqual(TEXT("entity count"), Swarm->GetEntityCount(), EntityCount);

	// 10 simulated seconds at 60 Hz — the F2 duration, deterministic.
	constexpr int32 Steps = 600;
	constexpr float StepSeconds = 1.0f / 60.0f;
	float WorstBatchMs = 0.0f;
	for (int32 S = 0; S < Steps; ++S)
	{
		Swarm->Step(StepSeconds);
		WorstBatchMs = FMath::Max(WorstBatchMs, Swarm->GetLastBatchMilliseconds());
	}

	// The cadence ran: ~10s / 0.5s interval, minus scheduling slack.
	TestTrue(FString::Printf(TEXT("batches ran (%d)"), Swarm->GetAssignmentBatchesRun()),
		Swarm->GetAssignmentBatchesRun() >= 18);

	// Every entity holds a valid assignment.
	int32 Invalid = 0;
	for (int32 E = 0; E < EntityCount; ++E)
	{
		int32 Archetype = INDEX_NONE;
		FVector2f Position;
		if (!Swarm->GetEntityState(E, Archetype, Position) || Archetype < 0 ||
			Archetype >= ArchetypeCount)
		{
			++Invalid;
		}
	}
	TestEqual(TEXT("entities with invalid assignment"), Invalid, 0);

	// Batch ceiling: 2,000 queries against a 64 x 16 int8 bank measured ~0.5 ms on
	// the reference machine; the ceiling is pinned an order of magnitude above so
	// the test guards regressions, not scheduler noise (E-group discipline).
	TestTrue(FString::Printf(TEXT("batch under ceiling (worst %.3f ms)"), WorstBatchMs),
		WorstBatchMs < 10.0f);
	AddInfo(FString::Printf(TEXT("F2: %d batches, worst %.3f ms"),
		Swarm->GetAssignmentBatchesRun(), WorstBatchMs));

	Swarm->StopSwarm();
	TestFalse(TEXT("swarm stopped"), Swarm->IsRunning());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
