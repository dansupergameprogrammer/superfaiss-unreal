#pragma once

#include "CoreMinimal.h"
#include "Subsystems/EngineSubsystem.h"

#include "SuperFAISSSubsystem.generated.h"

class USuperFAISSVectorBank;

USTRUCT(BlueprintType)
struct FSuperFAISSHit
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Similarity")
	int32 Index = -1;

	UPROPERTY(BlueprintReadOnly, Category = "Similarity")
	FName Id;

	UPROPERTY(BlueprintReadOnly, Category = "Similarity")
	float Score = 0.0f;
};

// C++ query options. Blueprint uses the simplified UFUNCTION surface below.
struct FSuperFAISSQueryArgs
{
	int32 K = 10;
	// Optional exclusion bitset, ceil(Count/32) words, bit set = skip row.
	TConstArrayView<uint32> ExcludeBits;
};

DECLARE_DYNAMIC_DELEGATE_TwoParams(FSuperFAISSResultDelegate,
	const TArray<FSuperFAISSHit>&, Hits, bool, bSuccess);

// Native (C++) completion delegate; the dynamic one above is the Blueprint face.
DECLARE_DELEGATE_TwoParams(FSuperFAISSNativeResultDelegate,
	const TArray<FSuperFAISSHit>& /*Hits*/, bool /*bSuccess*/);

// Handle for an in-flight async query. Cancel() is best-effort: a query that has not
// started computing is dropped; a running scan completes but its delegate reports
// bSuccess=false.
struct SUPERFAISSUNREAL_API FSuperFAISSTicket
{
	TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> CancelFlag;

	void Cancel()
	{
		if (CancelFlag.IsValid())
		{
			CancelFlag->store(true);
		}
	}
	bool IsValid() const { return CancelFlag.IsValid(); }
};

// The runtime query surface over baked banks: synchronous (any thread), asynchronous
// (task graph, bank pinned for the task's lifetime, delegate on the game thread), and
// batched (one bank pass for many queries). Queries are allocation-free once the
// subsystem's workspace pool is warm.
UCLASS()
class SUPERFAISSUNREAL_API USuperFAISSSubsystem : public UEngineSubsystem
{
	GENERATED_BODY()

public:
	// Blocking, callable from any thread. UnpaddedQuery has Bank->Dims elements; the
	// subsystem stages it into aligned padded scratch.
	bool QuerySync(
		const USuperFAISSVectorBank* Bank,
		TConstArrayView<float> UnpaddedQuery,
		const FSuperFAISSQueryArgs& Args,
		TArray<FSuperFAISSHit>& OutHits);

	// Dispatches to the task graph; the bank is pinned against GC until completion.
	// The delegate fires on the game thread with bSuccess=false on cancellation or
	// validation failure.
	FSuperFAISSTicket QueryAsync(
		const USuperFAISSVectorBank* Bank,
		TConstArrayView<float> UnpaddedQuery,
		const FSuperFAISSQueryArgs& Args,
		FSuperFAISSNativeResultDelegate Completion);

	// M queries in one bank pass (queries concatenated, stride Bank->Dims, unpadded).
	// OutHits is query-major, K hits per query (OutCounts holds per-query counts).
	bool QueryBatch(
		const USuperFAISSVectorBank* Bank,
		TConstArrayView<float> UnpaddedQueries,
		int32 QueryCount,
		const FSuperFAISSQueryArgs& Args,
		TArray<FSuperFAISSHit>& OutHits,
		TArray<int32>& OutCounts);

	// Blueprint surface. Sync is intended for small banks; Async for everything else.
	UFUNCTION(BlueprintCallable, Category = "Similarity",
		meta = (DisplayName = "Query Similar (Sync)"))
	bool QuerySimilarSync(const USuperFAISSVectorBank* Bank, const TArray<float>& Query,
		int32 K, TArray<FSuperFAISSHit>& Hits);

	UFUNCTION(BlueprintCallable, Category = "Similarity",
		meta = (DisplayName = "Query Similar (Async)"))
	void QuerySimilarAsync(const USuperFAISSVectorBank* Bank, const TArray<float>& Query,
		int32 K, FSuperFAISSResultDelegate OnComplete);

	// Diagnostics: workspace pool growth count (flat once warm — the B5 counter).
	uint64 GetPoolGrowthCount() const;

	// Drains in-flight async queries. The UObject exit purge runs before the task
	// graph shuts down, so a query in flight at engine exit would read freed bank
	// memory; Deinitialize runs before the purge and waits the fleet out.
	virtual void Deinitialize() override;

	// Pool scratch type; public so the shared RunQuery helper can take it.
	struct FPooledWorkspace;

private:

	TSharedPtr<FPooledWorkspace> AcquireWorkspace();
	void ReleaseWorkspace(TSharedPtr<FPooledWorkspace> Workspace);

	FCriticalSection PoolLock;
	TArray<TSharedPtr<FPooledWorkspace>> Pool;
	std::atomic<uint64> PoolGrowth{0};

	// In-flight async queries (dispatch through game-thread delivery). Drained by
	// Deinitialize; new dispatches during the drain fail immediately.
	std::atomic<int32> InFlightAsync{0};
	std::atomic<bool> bDraining{false};
};
