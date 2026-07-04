#pragma once

#include "Commandlets/Commandlet.h"

#include "SuperFAISSUnrealBakeCommandlet.generated.h"

// Imports a .wvbank sidecar pair into a saved USuperFAISSVectorBank asset:
//   UnrealEditor-Cmd <project> -run=SuperFAISSUnrealBake
//       -Source=<path>.wvbank.json -Package=/SuperFAISSUnreal/Demo/DemoBank
UCLASS()
class USuperFAISSUnrealBakeCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
