#include "SuperFAISSUnrealBakeCommandlet.h"

#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "SuperFAISSBankImport.h"
#include "SuperFAISSVectorBank.h"

int32 USuperFAISSUnrealBakeCommandlet::Main(const FString& Params)
{
	FString Source;
	FString PackagePath;
	FParse::Value(*Params, TEXT("Source="), Source);
	FParse::Value(*Params, TEXT("Package="), PackagePath);
	if (Source.IsEmpty() || PackagePath.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("usage: -run=SuperFAISSUnrealBake -Source=<sidecar.json> -Package=</Game/Path/Asset>"));
		return 1;
	}

	const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
	UPackage* Package = CreatePackage(*PackagePath);
	Package->FullyLoad();

	// Rebake over an existing asset: move the old object aside so the new one can
	// take its name.
	if (UObject* Existing = StaticFindObject(USuperFAISSVectorBank::StaticClass(), Package, *AssetName))
	{
		Existing->Rename(
			*MakeUniqueObjectName(GetTransientPackage(), Existing->GetClass()).ToString(),
			GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
		Existing->ClearFlags(RF_Public | RF_Standalone);
	}

	FString Error;
	USuperFAISSVectorBank* Bank = FSuperFAISSBankImport::Import(
		Source, Package, FName(*AssetName), ESuperFAISSBankQuantization::Int8, Error);
	if (Bank == nullptr)
	{
		UE_LOG(LogTemp, Error, TEXT("import failed: %s"), *Error);
		return 1;
	}

	const FString FileName = FPackageName::LongPackageNameToFilename(
		PackagePath, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	if (!UPackage::SavePackage(Package, Bank, *FileName, SaveArgs))
	{
		UE_LOG(LogTemp, Error, TEXT("save failed: %s"), *FileName);
		return 1;
	}

	UE_LOG(LogTemp, Display,
		TEXT("baked %s: %d x %d, %.1f MB payload, recall@10 %.4f -> %s"),
		*AssetName, Bank->Count, Bank->Dims,
		Bank->GetPayloadBytes() / (1024.0 * 1024.0), Bank->RecallAt10, *FileName);
	return 0;
}
