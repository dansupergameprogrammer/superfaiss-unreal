#include "SuperFAISSPrototypeAsset.h"

#include "SuperFAISSVectorBank.h"

bool USuperFAISSPrototypeAsset::GetCrossDeviceQuery(
	FSuperFAISSCrossDeviceQuery& OutQuery) const
{
	OutQuery = FSuperFAISSCrossDeviceQuery{};
	if (!IsCrossDeviceTier())
	{
		return false; // presentation tier: no quantized payload to read
	}
	// The REQUIRED version gate (FAI-1): an XD payload under the presentation
	// version is a defined rejection, never silently readable.
	if (AssetVersion < kAssetVersionCrossDevice || !CrossDeviceQuery.IsPayloadValid())
	{
		return false;
	}
	OutQuery = CrossDeviceQuery;
	return true;
}

void USuperFAISSPrototypeAsset::PostLoad()
{
	Super::PostLoad();
	if (IsCrossDeviceTier() && AssetVersion < kAssetVersionCrossDevice)
	{
		UE_LOG(LogTemp, Error,
			TEXT("SuperFAISSPrototypeAsset %s: cross-device payload without the ")
			TEXT("required asset version bump (%d < %d) - the payload is rejected"),
			*GetPathName(), AssetVersion, kAssetVersionCrossDevice);
	}
}

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
