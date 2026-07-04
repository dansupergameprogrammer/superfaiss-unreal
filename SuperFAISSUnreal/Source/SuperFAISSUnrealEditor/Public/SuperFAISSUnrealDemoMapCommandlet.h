#pragma once

#include "Commandlets/Commandlet.h"

#include "SuperFAISSUnrealDemoMapCommandlet.generated.h"

UCLASS()
class USuperFAISSUnrealDemoMapCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
