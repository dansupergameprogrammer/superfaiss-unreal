#pragma once

#include "CoreMinimal.h"
#include "Mass/EntityElementTypes.h"
#include "Mass/EntityHandle.h"
#include "Subsystems/WorldSubsystem.h"

#include "SuperFAISSSwarmSubsystem.generated.h"

class USuperFAISSVectorBank;

// Station 2 fragment: each swarm entity carries its preference-vector index, its
// current archetype assignment, and a 2D drift position for the debug render.
USTRUCT()
struct FSuperFAISSSwarmFragment : public FMassFragment
{
	GENERATED_BODY()

	int32 PreferenceIndex = INDEX_NONE;
	int32 Archetype = INDEX_NONE;
	FVector2f Position = FVector2f::ZeroVector;
};

// Station 2 (plan §11, D13): ~2,000 Mass entities carrying small preference vectors;
// a 64-entry archetype bank; a recurring batch query assigns each entity its nearest
// archetype; entities re-color and drift toward cluster anchors. This subsystem owns
// the swarm state and the batch cadence (the §13.3 pattern: gather → one QueryBatch →
// results land on entities next tick); USuperFAISSSwarmProcessor applies drift and
// draws. The demo module is the plugin's only Mass contact (§6).
UCLASS()
class SUPERFAISSUNREALDEMO_API USuperFAISSSwarmSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	// Builds the archetype bank (ArchetypeCount x Dims, Cosine/int8, seeded), the
	// per-entity preference vectors, and spawns the Mass entities. Idempotent:
	// restarting stops the previous swarm first.
	bool StartSwarm(int32 EntityCount, int32 ArchetypeCount, int32 Dims, int32 Seed);
	void StopSwarm();
	bool IsRunning() const { return Entities.Num() > 0; }

	// One simulation step: at the assignment cadence, run the batch and stage the
	// assignments; every step, write staged assignments into fragments (the
	// deferred landing) — drift itself runs in the Mass processor. Public so the F2
	// test drives the exact path Tick drives.
	void Step(float DeltaSeconds);

	// Readout (Station 2 on-screen panel + F2 assertions).
	int32 GetEntityCount() const { return Entities.Num(); }
	int32 GetArchetypeCount() const { return ArchetypeAnchors2D.Num(); }
	float GetLastBatchMilliseconds() const { return LastBatchMs; }
	int32 GetAssignmentBatchesRun() const { return BatchesRun; }
	FVector2f GetAnchor2D(int32 Archetype) const;

	// F2 support: read a fragment snapshot (assignment + position) per entity.
	bool GetEntityState(int32 EntityIndex, int32& OutArchetype, FVector2f& OutPosition) const;

	//~ UTickableWorldSubsystem
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(USuperFAISSSwarmSubsystem, STATGROUP_Tickables);
	}
	virtual bool IsTickable() const override { return IsRunning(); }
	virtual void Deinitialize() override;

	// Assignment cadence, seconds.
	static constexpr float AssignInterval = 0.5f;

private:
	void RunAssignmentBatch();

	UPROPERTY(Transient)
	TObjectPtr<USuperFAISSVectorBank> ArchetypeBank;

	// EntityCount x Dims preference vectors, row-major (static per swarm).
	TArray<float> Preferences;
	// Staged by RunAssignmentBatch, landed on fragments by the next Step.
	TArray<int32> StagedAssignments;
	bool bAssignmentsStaged = false;

	TArray<FMassEntityHandle> Entities;
	TArray<FVector2f> ArchetypeAnchors2D;

	float TimeSinceAssign = 0.0f;
	float LastBatchMs = 0.0f;
	int32 BatchesRun = 0;
	int32 PrefDims = 0;
};
