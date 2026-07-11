#include "SuperFAISSAuthoringLibrary.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "SuperFAISSPrototypeAsset.h"
#include "SuperFAISSSubsystem.h"
#include "SuperFAISSVectorBank.h"
#include "UObject/Package.h"

USuperFAISSPrototypeAsset* USuperFAISSAuthoringLibrary::CreatePrototypeAsset(
	const USuperFAISSVectorBank* Bank,
	const TArray<int32>& RowIndices,
	const TArray<FName>& RowIds,
	const FString& PackagePath,
	const FString& AssetName,
	FString& OutError)
{
	OutError.Reset();
	if (Bank == nullptr || !Bank->IsValid())
	{
		OutError = TEXT("invalid bank");
		return nullptr;
	}
	if (PackagePath.IsEmpty() || AssetName.IsEmpty())
	{
		OutError = TEXT("package path and asset name are required");
		return nullptr;
	}

	// Resolve the member set: ids first (when given and resolvable), indices after.
	TArray<int32> Rows;
	for (const FName& Id : RowIds)
	{
		const int32 Index = Bank->GetIndexForId(Id);
		if (Index == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("id not in bank: %s"), *Id.ToString());
			return nullptr;
		}
		Rows.AddUnique(Index);
	}
	for (const int32 Index : RowIndices)
	{
		if (Index < 0 || Index >= Bank->Count)
		{
			OutError = FString::Printf(TEXT("row out of range: %d"), Index);
			return nullptr;
		}
		Rows.AddUnique(Index);
	}
	if (Rows.Num() == 0)
	{
		OutError = TEXT("no rows selected");
		return nullptr;
	}

	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	TArray<float> Centroid;
	if (Subsystem == nullptr || !Subsystem->MakeCentroidQuery(Bank, Rows, Centroid))
	{
		OutError = TEXT("centroid failed (cancelling members on a Cosine bank?)");
		return nullptr;
	}

	// A colliding name would silently rename the incumbent (Poirot M1): refuse
	// instead, so re-authoring is an explicit delete-then-create.
	const FString FullPackageName = PackagePath / AssetName;
	if (StaticFindObject(USuperFAISSPrototypeAsset::StaticClass(), nullptr,
			*(FullPackageName + TEXT(".") + AssetName)) != nullptr)
	{
		OutError = FString::Printf(TEXT("asset already exists: %s"), *FullPackageName);
		return nullptr;
	}
	UPackage* Package = CreatePackage(*FullPackageName);
	if (Package == nullptr)
	{
		OutError = FString::Printf(TEXT("cannot create package %s"), *FullPackageName);
		return nullptr;
	}

	USuperFAISSPrototypeAsset* Asset = NewObject<USuperFAISSPrototypeAsset>(
		Package, FName(*AssetName), RF_Public | RF_Standalone);
	Asset->Query = MoveTemp(Centroid);
	Asset->SourceBankName = Bank->GetFName();
	Asset->SourceRows = MoveTemp(Rows);

	Asset->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(Asset);
	return Asset;
}

USuperFAISSPrototypeAsset* USuperFAISSAuthoringLibrary::CreatePrototypeAssetCrossDevice(
	const USuperFAISSVectorBank* Bank,
	const TArray<int32>& RowIndices,
	const TArray<FName>& RowIds,
	const TArray<int32>& Weights,
	const FString& PackagePath,
	const FString& AssetName,
	FString& OutError)
{
	OutError.Reset();
	if (Bank == nullptr || !Bank->IsValid())
	{
		OutError = TEXT("invalid bank");
		return nullptr;
	}
	if (Bank->Quantization != ESuperFAISSBankQuantization::Int8)
	{
		OutError = TEXT("cross-device prototypes require an int8 bank");
		return nullptr;
	}
	if (PackagePath.IsEmpty() || AssetName.IsEmpty())
	{
		OutError = TEXT("package path and asset name are required");
		return nullptr;
	}

	// Resolve the member set exactly as the float authoring path does: ids first
	// (when given and resolvable), indices after.
	TArray<int32> Rows;
	for (const FName& Id : RowIds)
	{
		const int32 Index = Bank->GetIndexForId(Id);
		if (Index == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("id not in bank: %s"), *Id.ToString());
			return nullptr;
		}
		Rows.AddUnique(Index);
	}
	for (const int32 Index : RowIndices)
	{
		if (Index < 0 || Index >= Bank->Count)
		{
			OutError = FString::Printf(TEXT("row out of range: %d"), Index);
			return nullptr;
		}
		Rows.AddUnique(Index);
	}
	if (Rows.Num() == 0)
	{
		OutError = TEXT("no rows selected");
		return nullptr;
	}
	if (Weights.Num() != 0 && Weights.Num() != Rows.Num())
	{
		OutError = FString::Printf(
			TEXT("weights (%d) must match the resolved rows (%d), or be empty"),
			Weights.Num(), Rows.Num());
		return nullptr;
	}

	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (Subsystem == nullptr)
	{
		OutError = TEXT("subsystem unavailable");
		return nullptr;
	}

	// The twin law: the baked anchor IS the runtime operator's product — the same
	// core MakeCentroidCrossDevice the runtime pool calls, no second math.
	FSuperFAISSCrossDeviceQuery Pooled;
	if (!Subsystem->MakeCentroidQueryCrossDevice(Bank, Rows, Weights, Pooled))
	{
		OutError = TEXT("cross-device pooling failed (cancelling members?)");
		return nullptr;
	}
	// The float presentation form beside it, so the asset stays a query provider.
	TArray<float> Centroid;
	if (!Subsystem->MakeCentroidQuery(Bank, Rows, Centroid))
	{
		OutError = TEXT("presentation centroid failed");
		return nullptr;
	}

	const FString FullPackageName = PackagePath / AssetName;
	if (StaticFindObject(USuperFAISSPrototypeAsset::StaticClass(), nullptr,
			*(FullPackageName + TEXT(".") + AssetName)) != nullptr)
	{
		OutError = FString::Printf(TEXT("asset already exists: %s"), *FullPackageName);
		return nullptr;
	}
	UPackage* Package = CreatePackage(*FullPackageName);
	if (Package == nullptr)
	{
		OutError = FString::Printf(TEXT("cannot create package %s"), *FullPackageName);
		return nullptr;
	}

	USuperFAISSPrototypeAsset* Asset = NewObject<USuperFAISSPrototypeAsset>(
		Package, FName(*AssetName), RF_Public | RF_Standalone);
	Asset->Query = MoveTemp(Centroid);
	Asset->SourceBankName = Bank->GetFName();
	Asset->SourceRows = MoveTemp(Rows);
	Asset->CrossDeviceQuery = MoveTemp(Pooled);
	// The REQUIRED minor-version bump for the cross-device tier (FAI-1).
	Asset->AssetVersion = USuperFAISSPrototypeAsset::kAssetVersionCrossDevice;

	Asset->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(Asset);
	return Asset;
}
