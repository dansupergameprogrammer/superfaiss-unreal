// Creates the demo map asset: an empty world whose game mode is the demo game mode.
//   UnrealEditor-Cmd <project> -run=SuperFAISSUnrealDemoMap
// (Editor module so the demo module itself stays editor-free.)

#include "SuperFAISSUnrealDemoMapCommandlet.h"

#include "Engine/World.h"
#include "Factories/WorldFactory.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

int32 USuperFAISSUnrealDemoMapCommandlet::Main(const FString& Params)
{
	const FString PackagePath = TEXT("/SuperFAISSUnreal/Demo/SimilarityDemo");

	UPackage* Package = CreatePackage(*PackagePath);
	Package->FullyLoad();

	UWorldFactory* Factory = NewObject<UWorldFactory>();
	UWorld* World = CastChecked<UWorld>(Factory->FactoryCreateNew(
		UWorld::StaticClass(), Package, TEXT("SimilarityDemo"),
		RF_Public | RF_Standalone, nullptr, GWarn));

	// The demo game mode is looked up by path so this editor module does not link the
	// demo module.
	UClass* GameModeClass = LoadClass<AGameModeBase>(nullptr,
		TEXT("/Script/SuperFAISSUnrealDemo.SuperFAISSDemoGameMode"));
	if (GameModeClass == nullptr)
	{
		UE_LOG(LogTemp, Error, TEXT("demo game mode class not found"));
		return 1;
	}
	World->GetWorldSettings()->DefaultGameMode = GameModeClass;

	FActorSpawnParameters Spawn;
	Spawn.OverrideLevel = World->PersistentLevel;
	World->SpawnActor<APlayerStart>(FVector::ZeroVector, FRotator::ZeroRotator, Spawn);

	const FString FileName = FPackageName::LongPackageNameToFilename(
		PackagePath, FPackageName::GetMapPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	if (!UPackage::SavePackage(Package, World, *FileName, SaveArgs))
	{
		UE_LOG(LogTemp, Error, TEXT("save failed: %s"), *FileName);
		return 1;
	}
	UE_LOG(LogTemp, Display, TEXT("demo map saved: %s"), *FileName);
	return 0;
}
