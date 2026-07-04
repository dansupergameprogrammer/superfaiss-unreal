#include "SuperFAISSSwarmProcessor.h"

#include "DrawDebugHelpers.h"
#include "MassExecutionContext.h"
#include "SuperFAISSSwarmSubsystem.h"

USuperFAISSSwarmProcessor::USuperFAISSSwarmProcessor()
	: EntityQuery(*this)
{
	ExecutionFlags = static_cast<int32>(EProcessorExecutionFlags::All);
	bAutoRegisterWithProcessingPhases = true;
}

void USuperFAISSSwarmProcessor::ConfigureQueries(
	const TSharedRef<FMassEntityManager>& EntityManager)
{
	EntityQuery.AddRequirement<FSuperFAISSSwarmFragment>(EMassFragmentAccess::ReadWrite);
}

void USuperFAISSSwarmProcessor::Execute(
	FMassEntityManager& EntityManager, FMassExecutionContext& Context)
{
	UWorld* World = EntityManager.GetWorld();
	USuperFAISSSwarmSubsystem* Swarm =
		World ? World->GetSubsystem<USuperFAISSSwarmSubsystem>() : nullptr;
	if (Swarm == nullptr || !Swarm->IsRunning())
	{
		return;
	}
	const float Delta = Context.GetDeltaTimeSeconds();
	const int32 ArchetypeCount = Swarm->GetArchetypeCount();

	EntityQuery.ForEachEntityChunk(Context,
		[Swarm, World, Delta, ArchetypeCount](FMassExecutionContext& ChunkContext)
		{
			const TArrayView<FSuperFAISSSwarmFragment> Fragments =
				ChunkContext.GetMutableFragmentView<FSuperFAISSSwarmFragment>();
			for (FSuperFAISSSwarmFragment& Fragment : Fragments)
			{
				if (Fragment.Archetype == INDEX_NONE)
				{
					continue;
				}
				const FVector2f Anchor = Swarm->GetAnchor2D(Fragment.Archetype);
				Fragment.Position += (Anchor - Fragment.Position) *
					FMath::Clamp(Delta * 2.0f, 0.0f, 1.0f);
#if ENABLE_DRAW_DEBUG
				const float Hue = ArchetypeCount > 0
					? 255.0f * Fragment.Archetype / ArchetypeCount
					: 0.0f;
				DrawDebugPoint(World,
					FVector(Fragment.Position.X, Fragment.Position.Y, 120.0f), 6.0f,
					FLinearColor::MakeFromHSV8(static_cast<uint8>(Hue), 200, 255)
						.ToFColor(true),
					false, -1.0f);
#endif
			}
		});
}
