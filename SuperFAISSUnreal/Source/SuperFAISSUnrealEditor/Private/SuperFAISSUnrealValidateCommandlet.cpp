#include "SuperFAISSUnrealValidateCommandlet.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "SuperFAISSBankImport.h"
#include "SuperFAISSVectorBank.h"

int32 USuperFAISSUnrealValidateCommandlet::Main(const FString& Params)
{
	FAssetRegistryModule& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	AssetRegistry.Get().SearchAllAssets(/*bSynchronousSearch*/ true);

	TArray<FAssetData> Assets;
	AssetRegistry.Get().GetAssetsByClass(
		USuperFAISSVectorBank::StaticClass()->GetClassPathName(), Assets);

	// Loading runs PostLoad, which validates; ValidateLoadedBanks then sweeps.
	for (const FAssetData& Asset : Assets)
	{
		Asset.GetAsset();
	}

	TArray<FString> Invalid;
	const int32 InvalidCount = FSuperFAISSBankImport::ValidateLoadedBanks(Invalid);
	for (const FString& Path : Invalid)
	{
		UE_LOG(LogTemp, Error, TEXT("invalid bank: %s"), *Path);
	}
	UE_LOG(LogTemp, Display, TEXT("SuperFAISSUnrealValidate: %d banks, %d invalid"),
		Assets.Num(), InvalidCount);
	return InvalidCount == 0 ? 0 : 1;
}
