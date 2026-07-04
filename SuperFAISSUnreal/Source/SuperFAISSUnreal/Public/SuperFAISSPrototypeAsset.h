#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "SuperFAISSQueryProvider.h"

#include "SuperFAISSPrototypeAsset.generated.h"

// A named prototype query ("the category's center"): the centroid of selected bank
// rows, saved as a small asset for 18.1 prototype scoring. Authored in-editor
// (SuperFAISSUnrealEditor authoring surface) from a bank and a row selection; usable
// anywhere a query vector is — and directly as a query provider (18.4), so gameplay
// can hand the asset itself to the subsystem path.
UCLASS(BlueprintType)
class SUPERFAISSUNREAL_API USuperFAISSPrototypeAsset
	: public UDataAsset
	, public ISuperFAISSQueryProvider
{
	GENERATED_BODY()

public:
	// The prototype vector (unpadded, Dims elements). On Cosine-bank prototypes this
	// is unit norm (the centroid path renormalizes).
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Similarity")
	TArray<float> Query;

	// Provenance, for the linter's category analyses and for humans: the bank the
	// prototype was authored from and the member rows.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Similarity")
	FName SourceBankName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Similarity")
	TArray<int32> SourceRows;

	//~ ISuperFAISSQueryProvider: the prototype is the query, valid against any bank
	// of matching dimensionality.
	virtual bool GetQueryVector_Implementation(
		const USuperFAISSVectorBank* Bank, TArray<float>& OutQuery) override;
};
