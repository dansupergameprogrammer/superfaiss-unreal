#pragma once

#include "Commandlets/Commandlet.h"

#include "SuperFAISSUnrealValidateCommandlet.generated.h"

// Loads and validates every USuperFAISSVectorBank asset in the project; non-zero exit on
// any invalid bank. CI-callable:
//   UnrealEditor-Cmd <project> -run=SuperFAISSUnrealValidate
UCLASS()
class USuperFAISSUnrealValidateCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
