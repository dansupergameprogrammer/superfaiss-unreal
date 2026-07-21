#include "SuperFAISSQueryProvider.h"

#include "SuperFAISSSubsystem.h"
#include "SuperFAISSVectorBank.h"

bool USuperFAISSBankRowQueryProvider::GetQueryVector_Implementation(
	const USuperFAISSVectorBank* Bank, TArray<float>& OutQuery)
{
	OutQuery.Reset();
	if (Bank == nullptr || !Bank->IsValid())
	{
		return false;
	}

	// Id semantics are strict (O2): a set-but-unresolved id fails rather
	// than silently querying RowIndex.
	int32 Row = INDEX_NONE;
	if (!RowId.IsNone())
	{
		Row = Bank->GetIndexForId(RowId);
		if (Row == INDEX_NONE)
		{
			return false;
		}
	}
	else
	{
		Row = RowIndex;
	}
	if (Row < 0 || Row >= Bank->Count)
	{
		return false;
	}

	// A one-row centroid is the row itself, dequantized (and, on Cosine banks,
	// re-unit-normalized — stored rows are already unit norm, so this is a no-op
	// up to rounding).
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (Subsystem == nullptr)
	{
		return false;
	}
	return Subsystem->MakeCentroidQuery(Bank, {Row}, OutQuery);
}
