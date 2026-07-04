#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"

#include "SuperFAISSQueryProvider.generated.h"

class USuperFAISSVectorBank;

// The query side of the encoder seam (plugin plan 18.4). An encoder is anything that
// turns domain state into a query vector; this contract is where it plugs in. The
// plugin ships the contract and one reference provider — it ships NO domain encoders:
// everything domain-specific (what the vector means, where it comes from) lives on the
// implementing side. Consumers implement this on any UObject — a component, an actor,
// a data asset — in C++ or Blueprint, and hand the result to USuperFAISSSubsystem.
UINTERFACE(BlueprintType, MinimalAPI)
class USuperFAISSQueryProvider : public UInterface
{
	GENERATED_BODY()
};

class SUPERFAISSUNREAL_API ISuperFAISSQueryProvider
{
	GENERATED_BODY()

public:
	// Produce the query vector to run against Bank. OutQuery must have Bank->Dims
	// elements on success. Return false when no query is available (nothing selected,
	// source data missing) — callers treat false as "do not query", not as an error.
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Similarity")
	bool GetQueryVector(const USuperFAISSVectorBank* Bank, TArray<float>& OutQuery);
};

// Reference provider: the query is an existing bank row, chosen by Id (when the bank
// carries ids) or by index. This is the "find things like this known thing" case —
// the demo's typed-word mode as a reusable object, and the worked example for
// implementing the contract. Id takes precedence when both are set.
UCLASS(BlueprintType, EditInlineNew)
class SUPERFAISSUNREAL_API USuperFAISSBankRowQueryProvider
	: public UObject
	, public ISuperFAISSQueryProvider
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Similarity")
	FName RowId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Similarity")
	int32 RowIndex = INDEX_NONE;

	virtual bool GetQueryVector_Implementation(
		const USuperFAISSVectorBank* Bank, TArray<float>& OutQuery) override;
};
