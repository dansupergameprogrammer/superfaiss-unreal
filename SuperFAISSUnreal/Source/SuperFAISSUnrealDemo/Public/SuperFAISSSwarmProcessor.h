#pragma once

#include "CoreMinimal.h"
#include "MassProcessor.h"
#include "MassEntityQuery.h" // FMassEntityQuery held by value below; MassProcessor.h only forward-declares it (Shipping builds have no PCH to mask the omission)

#include "SuperFAISSSwarmProcessor.generated.h"

// Station 2 drift: every frame, each swarm entity moves toward the 2D anchor of its
// assigned archetype and draws itself colored by assignment. Assignment itself lives
// in USuperFAISSSwarmSubsystem (the batch cadence); this processor only consumes the
// fragment state — the Mass side of the §13.3 split.
UCLASS()
class SUPERFAISSUNREALDEMO_API USuperFAISSSwarmProcessor : public UMassProcessor
{
	GENERATED_BODY()

public:
	USuperFAISSSwarmProcessor();

protected:
	virtual void ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager) override;
	virtual void Execute(FMassEntityManager& EntityManager,
		FMassExecutionContext& Context) override;

	FMassEntityQuery EntityQuery;
};
