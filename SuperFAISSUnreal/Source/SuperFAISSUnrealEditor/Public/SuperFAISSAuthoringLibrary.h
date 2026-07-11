#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"

#include "SuperFAISSAuthoringLibrary.generated.h"

class USuperFAISSPrototypeAsset;
class USuperFAISSVectorBank;

// Editor authoring surface (plan 18.2): selected bank rows become a named prototype
// query asset (18.1 prototype scoring; also a query provider, 18.4). Callable from
// editor scripting and editor-utility Blueprints.
UCLASS()
class SUPERFAISSUNREALEDITOR_API USuperFAISSAuthoringLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// Creates (or replaces) a prototype asset at PackagePath/AssetName from the
	// centroid of RowIndices. Rows may be given by id instead via RowIds (ids win
	// when both are set and resolvable). Returns nullptr with OutError on failure —
	// including a cancelling (zero-norm) centroid on Cosine banks.
	UFUNCTION(BlueprintCallable, Category = "Similarity|Editor")
	static USuperFAISSPrototypeAsset* CreatePrototypeAsset(
		const USuperFAISSVectorBank* Bank,
		const TArray<int32>& RowIndices,
		const TArray<FName>& RowIds,
		const FString& PackagePath,
		const FString& AssetName,
		FString& OutError);

	// Cross-device-tier prototype authoring (v2.4, the bake-side twin entry point):
	// bakes the recognizer anchor with the SAME integer-domain operator the runtime
	// uses (USuperFAISSSubsystem::MakeCentroidQueryCrossDevice) — one operator, two
	// entry points, no second math — so the stored quantized centroid byte-equals a
	// runtime pool over identical rows. The asset takes the REQUIRED cross-device
	// version bump; the float presentation Query is baked beside it (the provider
	// path keeps working). int8 banks only. Weights: empty = unweighted, else one
	// positive integer per resolved row (ids resolve first, indices after — the
	// weight order matches the RESOLVED row order).
	UFUNCTION(BlueprintCallable, Category = "Similarity|Editor")
	static USuperFAISSPrototypeAsset* CreatePrototypeAssetCrossDevice(
		const USuperFAISSVectorBank* Bank,
		const TArray<int32>& RowIndices,
		const TArray<FName>& RowIds,
		const TArray<int32>& Weights,
		const FString& PackagePath,
		const FString& AssetName,
		FString& OutError);
};
