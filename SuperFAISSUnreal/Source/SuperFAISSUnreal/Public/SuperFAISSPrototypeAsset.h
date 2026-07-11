#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "SuperFAISSQueryProvider.h"
#include "SuperFAISSSubsystem.h"

#include "SuperFAISSPrototypeAsset.generated.h"

// A named prototype query ("the category's center"): the centroid of selected bank
// rows, saved as a small asset for 18.1 prototype scoring. Authored in-editor
// (SuperFAISSUnrealEditor authoring surface) from a bank and a row selection; usable
// anywhere a query vector is — and directly as a query provider (18.4), so gameplay
// can hand the asset itself to the subsystem path.
//
// Tiers (v2.4, plan section 21 FAI-1): the float Query is the PRESENTATION tier —
// unchanged, cross-device-honest only up to float pooling. A cross-device-tier
// asset additionally stores the QUANTIZED centroid the integer-domain operator
// produced (CrossDeviceQuery), on which the baked-anchor == runtime-pooled twin
// equality is bitwise. The cross-device tier REQUIRES the bumped AssetVersion:
// reading an XD payload under the presentation version is a defined rejection.
UCLASS(BlueprintType)
class SUPERFAISSUNREAL_API USuperFAISSPrototypeAsset
	: public UDataAsset
	, public ISuperFAISSQueryProvider
{
	GENERATED_BODY()

public:
	// Asset format versions: 1 = float presentation form; 2 = may carry the
	// cross-device quantized centroid. The bump is REQUIRED for the cross-device
	// tier — a required bump, not an option (FAI-1).
	static constexpr int32 kAssetVersionPresentation = 1;
	static constexpr int32 kAssetVersionCrossDevice = 2;

	// The prototype vector (unpadded, Dims elements). On Cosine-bank prototypes this
	// is unit norm (the centroid path renormalizes). Presentation tier.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Similarity")
	TArray<float> Query;

	// Provenance, for the linter's category analyses and for humans: the bank the
	// prototype was authored from and the member rows.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Similarity")
	FName SourceBankName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Similarity")
	TArray<int32> SourceRows;

	// Format version of this asset (see the constants above). Pre-v2.4 assets
	// deserialize without the property and read as the presentation version.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Similarity")
	int32 AssetVersion = kAssetVersionPresentation;

	// Cross-device tier payload (empty on presentation-tier assets): the quantized
	// centroid MakeCentroidQueryCrossDevice produced over SourceRows — the
	// operator's product, stored so the baked anchor byte-equals the runtime pool.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Similarity")
	FSuperFAISSCrossDeviceQuery CrossDeviceQuery;

	UFUNCTION(BlueprintPure, Category = "Similarity")
	bool IsCrossDeviceTier() const { return CrossDeviceQuery.ImageQ8.Num() > 0; }

	// The stored quantized centroid, gated by the REQUIRED version bump: an XD
	// payload under a version below kAssetVersionCrossDevice is a defined
	// rejection — never silently readable (FAI-1). False on presentation-tier
	// assets (no payload to read).
	UFUNCTION(BlueprintPure, Category = "Similarity")
	bool GetCrossDeviceQuery(FSuperFAISSCrossDeviceQuery& OutQuery) const;

	// Flags the version-gate violation at load (the defined rejection surfaces in
	// the log; GetCrossDeviceQuery enforces it at every read).
	virtual void PostLoad() override;

	//~ ISuperFAISSQueryProvider: the prototype is the query, valid against any bank
	// of matching dimensionality.
	virtual bool GetQueryVector_Implementation(
		const USuperFAISSVectorBank* Bank, TArray<float>& OutQuery) override;
};
