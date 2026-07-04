#include "SuperFAISSPrototypeAsset.h"

#include "SuperFAISSVectorBank.h"

bool USuperFAISSPrototypeAsset::GetQueryVector_Implementation(
	const USuperFAISSVectorBank* Bank, TArray<float>& OutQuery)
{
	OutQuery.Reset();
	if (Query.Num() == 0 || Bank == nullptr || !Bank->IsValid() ||
		Bank->Dims != Query.Num())
	{
		return false;
	}
	OutQuery = Query;
	return true;
}
