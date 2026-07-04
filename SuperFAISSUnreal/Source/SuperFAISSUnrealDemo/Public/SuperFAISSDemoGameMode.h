#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"

#include "SuperFAISSDemoGameMode.generated.h"

// Demo game mode: loads the demo bank and puts the station UI on screen.
// The demo map (/SuperFAISSUnreal/Demo/SimilarityDemo) sets this as its game mode;
// it also works dropped into any map's World Settings.
UCLASS()
class SUPERFAISSUNREALDEMO_API ASuperFAISSDemoGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	// Hard references: the widget holds banks weakly, so without this UPROPERTY the
	// GC collects them mid-session (LoadObject grants no protection). The async query
	// pin covers in-flight scans only — ownership is the caller's job, and here the
	// game mode is the owner. Populated by asset-registry discovery: any
	// USuperFAISSVectorBank under /SuperFAISSUnreal/Demo appears in the switcher.
	UPROPERTY()
	TArray<TObjectPtr<class USuperFAISSVectorBank>> DemoBanks;

	TSharedPtr<class SSuperFAISSUnrealDemo> DemoWidget;
};
