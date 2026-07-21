#include "SuperFAISSSwarmSubsystem.h"

#include "MassEntityManager.h"
#include "MassEntitySubsystem.h"
#include "MassExecutionContext.h"
#include "SuperFAISSSubsystem.h"
#include "SuperFAISSVectorBank.h"

namespace
{
	// Deterministic xorshift matching the test-suite convention.
	struct FSwarmRng
	{
		uint64 State;
		explicit FSwarmRng(uint64 Seed) : State(Seed ? Seed : 0x9E3779B97F4A7C15ull) {}
		uint64 Next()
		{
			State ^= State >> 12;
			State ^= State << 25;
			State ^= State >> 27;
			return State * 0x2545F4914F6CDD1Dull;
		}
		float NextFloat() // [-1, 1) — centered (O1)
		{
			return static_cast<float>(static_cast<int64>(Next() >> 24)) /
				static_cast<float>(1ll << 39) - 1.0f;
		}
	};
}

bool USuperFAISSSwarmSubsystem::StartSwarm(
	int32 EntityCount, int32 ArchetypeCount, int32 Dims, int32 Seed)
{
	StopSwarm();
	if (EntityCount <= 0 || ArchetypeCount <= 0 || Dims <= 0)
	{
		return false;
	}
	UMassEntitySubsystem* MassSubsystem =
		GetWorld() ? GetWorld()->GetSubsystem<UMassEntitySubsystem>() : nullptr;
	if (MassSubsystem == nullptr)
	{
		return false;
	}

	FSwarmRng Rng(static_cast<uint64>(Seed) * 0x9E3779B97F4A7C15ull + 1);

	// Archetype bank: ArchetypeCount random unit-ish rows, Cosine/int8 — baked
	// through the real init path so Station 2 exercises the same asset machinery.
	TArray<float> AnchorRows;
	AnchorRows.SetNumUninitialized(ArchetypeCount * Dims);
	for (float& V : AnchorRows)
	{
		V = Rng.NextFloat();
	}
	ArchetypeBank = NewObject<USuperFAISSVectorBank>(this);
	FString Error;
	if (!ArchetypeBank->InitFromSource(AnchorRows, ArchetypeCount, Dims,
		ESuperFAISSBankMetric::Cosine, ESuperFAISSBankQuantization::Int8, {},
		TEXT("swarm-archetypes"), Error))
	{
		UE_LOG(LogTemp, Error, TEXT("SuperFAISS swarm: archetype bank failed: %s"), *Error);
		ArchetypeBank = nullptr;
		return false;
	}

	// Screen-space anchors on a circle, one per archetype.
	ArchetypeAnchors2D.SetNum(ArchetypeCount);
	for (int32 A = 0; A < ArchetypeCount; ++A)
	{
		const float Angle = 2.0f * PI * A / ArchetypeCount;
		ArchetypeAnchors2D[A] = FVector2f(FMath::Cos(Angle), FMath::Sin(Angle)) * 400.0f;
	}

	// Per-entity preference vectors (static for the swarm's lifetime).
	PrefDims = Dims;
	Preferences.SetNumUninitialized(EntityCount * Dims);
	for (float& V : Preferences)
	{
		V = Rng.NextFloat();
	}

	// Spawn the entities.
	FMassEntityManager& Manager = MassSubsystem->GetMutableEntityManager();
	const FMassArchetypeHandle Archetype = Manager.CreateArchetype(
		TConstArrayView<const UScriptStruct*>{FSuperFAISSSwarmFragment::StaticStruct()});
	Entities.Reserve(EntityCount);
	for (int32 E = 0; E < EntityCount; ++E)
	{
		const FMassEntityHandle Handle = Manager.CreateEntity(Archetype);
		Entities.Add(Handle);
		FSuperFAISSSwarmFragment& Fragment =
			Manager.GetFragmentDataChecked<FSuperFAISSSwarmFragment>(Handle);
		Fragment.PreferenceIndex = E;
		Fragment.Archetype = INDEX_NONE;
		Fragment.Position = FVector2f(Rng.NextFloat(), Rng.NextFloat()) * 400.0f;
	}

	TimeSinceAssign = AssignInterval; // first Step runs a batch immediately
	LastBatchMs = 0.0f;
	BatchesRun = 0;
	bAssignmentsStaged = false;
	return true;
}

void USuperFAISSSwarmSubsystem::StopSwarm()
{
	if (Entities.Num() > 0)
	{
		UMassEntitySubsystem* MassSubsystem =
			GetWorld() ? GetWorld()->GetSubsystem<UMassEntitySubsystem>() : nullptr;
		if (MassSubsystem != nullptr)
		{
			FMassEntityManager& Manager = MassSubsystem->GetMutableEntityManager();
			for (const FMassEntityHandle& Handle : Entities)
			{
				if (Manager.IsEntityValid(Handle))
				{
					Manager.DestroyEntity(Handle);
				}
			}
		}
	}
	Entities.Reset();
	Preferences.Reset();
	StagedAssignments.Reset();
	ArchetypeAnchors2D.Reset();
	ArchetypeBank = nullptr;
	bAssignmentsStaged = false;
}

void USuperFAISSSwarmSubsystem::RunAssignmentBatch()
{
	USuperFAISSSubsystem* Similarity = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (Similarity == nullptr || ArchetypeBank == nullptr)
	{
		return;
	}
	const int32 EntityCount = Entities.Num();

	FSuperFAISSQueryArgs Args;
	Args.K = 1;
	TArray<FSuperFAISSHit> Hits;
	TArray<int32> Counts;
	const double T0 = FPlatformTime::Seconds();
	const bool bOk =
		Similarity->QueryBatch(ArchetypeBank, Preferences, EntityCount, Args, Hits, Counts);
	LastBatchMs = static_cast<float>((FPlatformTime::Seconds() - T0) * 1000.0);
	if (!bOk)
	{
		return;
	}
	StagedAssignments.SetNumUninitialized(EntityCount);
	for (int32 E = 0; E < EntityCount; ++E)
	{
		StagedAssignments[E] = Counts[E] > 0 ? Hits[E].Index : INDEX_NONE;
	}
	bAssignmentsStaged = true;
	++BatchesRun;
}

void USuperFAISSSwarmSubsystem::Step(float DeltaSeconds)
{
	if (!IsRunning())
	{
		return;
	}

	// Land staged assignments from the previous batch (the §13.3 "results land next
	// tick" side of the pattern).
	if (bAssignmentsStaged)
	{
		UMassEntitySubsystem* MassSubsystem =
			GetWorld() ? GetWorld()->GetSubsystem<UMassEntitySubsystem>() : nullptr;
		if (MassSubsystem != nullptr)
		{
			FMassEntityManager& Manager = MassSubsystem->GetMutableEntityManager();
			for (int32 E = 0; E < Entities.Num(); ++E)
			{
				if (Manager.IsEntityValid(Entities[E]))
				{
					Manager.GetFragmentDataChecked<FSuperFAISSSwarmFragment>(Entities[E])
						.Archetype = StagedAssignments[E];
				}
			}
		}
		bAssignmentsStaged = false;
	}

	TimeSinceAssign += DeltaSeconds;
	if (TimeSinceAssign >= AssignInterval)
	{
		TimeSinceAssign = 0.0f;
		RunAssignmentBatch();
	}
}

void USuperFAISSSwarmSubsystem::Tick(float DeltaTime)
{
	Step(DeltaTime);
}

FVector2f USuperFAISSSwarmSubsystem::GetAnchor2D(int32 Archetype) const
{
	return ArchetypeAnchors2D.IsValidIndex(Archetype) ? ArchetypeAnchors2D[Archetype]
	                                                  : FVector2f::ZeroVector;
}

bool USuperFAISSSwarmSubsystem::GetEntityState(
	int32 EntityIndex, int32& OutArchetype, FVector2f& OutPosition) const
{
	OutArchetype = INDEX_NONE;
	OutPosition = FVector2f::ZeroVector;
	if (!Entities.IsValidIndex(EntityIndex))
	{
		return false;
	}
	UMassEntitySubsystem* MassSubsystem =
		GetWorld() ? GetWorld()->GetSubsystem<UMassEntitySubsystem>() : nullptr;
	if (MassSubsystem == nullptr)
	{
		return false;
	}
	FMassEntityManager& Manager = MassSubsystem->GetMutableEntityManager();
	if (!Manager.IsEntityValid(Entities[EntityIndex]))
	{
		return false;
	}
	const FSuperFAISSSwarmFragment& Fragment =
		Manager.GetFragmentDataChecked<FSuperFAISSSwarmFragment>(Entities[EntityIndex]);
	OutArchetype = Fragment.Archetype;
	OutPosition = Fragment.Position;
	return true;
}

void USuperFAISSSwarmSubsystem::Deinitialize()
{
	StopSwarm();
	Super::Deinitialize();
}
