#include "SuperFAISSSubsystem.h"

#include "SuperFAISSScratchBank.h"
#include "Misc/ScopeExit.h"

#include "Async/ParallelFor.h"
#include "Async/TaskGraphInterfaces.h"
#include "HAL/IConsoleManager.h"
#include "Stats/Stats.h"
#include "UObject/StrongObjectPtr.h"
#include "SuperFAISSVectorBank.h"

#include "superfaiss/superfaiss.h"

// Parallel-chunk scan selection (plan §6/§13.5): 0 = serial, 1 = auto (parallel when
// the bank has at least superfaiss.ParallelScan.MinChunks chunks), 2 = force parallel.
// Serial and parallel results are bit-identical (the top-k comparator is a strict
// total order; enforced by SuperFAISS.B.SerialParallelEquality).
static TAutoConsoleVariable<int32> CVarSuperFAISSParallelScan(
	TEXT("superfaiss.ParallelScan"), 1,
	TEXT("0=serial, 1=auto, 2=force parallel"));
static TAutoConsoleVariable<int32> CVarSuperFAISSParallelMinChunks(
	TEXT("superfaiss.ParallelScan.MinChunks"), 8,
	TEXT("Minimum chunk count before the auto mode parallelizes a scan"));

DECLARE_STATS_GROUP(TEXT("SuperFAISSUnreal"), STATGROUP_SuperFAISS, STATCAT_Advanced);
DECLARE_CYCLE_STAT(TEXT("QuerySync"), STAT_SuperFAISSQuerySync, STATGROUP_SuperFAISS);
DECLARE_CYCLE_STAT(TEXT("QueryBatch"), STAT_SuperFAISSQueryBatch, STATGROUP_SuperFAISS);
DECLARE_CYCLE_STAT(TEXT("QueryAsyncTask"), STAT_SuperFAISSQueryAsync, STATGROUP_SuperFAISS);
DECLARE_CYCLE_STAT(TEXT("QueryIntersect"), STAT_SuperFAISSQueryIntersect, STATGROUP_SuperFAISS);

// Per-task scratch: a core Workspace (single-owner, per the core contract) plus an
// aligned staging buffer for padded queries. Pooled; a warm pool never allocates.
struct USuperFAISSSubsystem::FPooledWorkspace
{
	superfaiss::Workspace Core;
	TArray<float, TAlignedHeapAllocator<16>> QueryStaging;
	TArray<superfaiss::Hit> HitStaging;
	TArray<int32_t> CountStaging;
	// Scratch-bank exclusion staging: snapshot tombstones (OR'd with the caller's
	// excludeBits) live here so the scratch path stays allocation-free once warm.
	TArray<uint32> TombstoneStaging;
	// v2.4 pooled cross-device staging: the operator writes (and QueryXd reads) the
	// int8 image through this aligned buffer — the payload struct's TArray<uint8>
	// carries no alignment guarantee, the core requires kAlignment.
	TArray<int8, TAlignedHeapAllocator<16>> XdImageStaging;
	// v2.5 analytics staging (plan section 22): a SECOND aligned int8 image so a pair
	// score / set-to-set distance holds both operands' images (or both pooled
	// centroids) aligned at once; and an XdQuery array for the NN reductions' lifted
	// source rows (MeanNN/MaxNN scratch of source.count). Both from the pooled
	// workspace so the analytics path stays allocation-free once warm.
	TArray<int8, TAlignedHeapAllocator<16>> XdImageStagingB;
	TArray<superfaiss::XdQuery> XdQueryStaging;
	// v2.5 scratch-source analytics: the snapshot BankView's live rows against a baked
	// target need the snapshot's float direction / padded staging reused from the
	// buffers above; the tombstone words reuse TombstoneStaging (as QueryScratch does).

	// Parallel-scan scratch, all from this one pooled workspace so the parallel path
	// stays allocation-free once warm (Poirot T-033 constraint): per-chunk heap and
	// finalized-list storage, list pointers/counts for the merge, and the merge heap.
	TArray<superfaiss::Hit> ChunkHeapStorage;
	TArray<superfaiss::Hit> ChunkSorted;
	TArray<const superfaiss::Hit*> ChunkListPtrs;
	TArray<int32_t> ChunkListCounts;
	TArray<int32_t> MergeCountScratch;
	TArray<superfaiss::Hit> MergeHeap;
};

namespace
{
	// Resolves Args.Channels / Args.Segments to a core segment list against the
	// bank (names resolve here, never in the kernel - plan section 5). Returns
	// false on an unresolvable name, both forms set, or malformed ranges (the core
	// re-validates ranges; this catches the name layer).
	bool ResolveSegments(
		const USuperFAISSVectorBank* Bank,
		const FSuperFAISSQueryArgs& Args,
		TArray<superfaiss::QuerySegment>& OutSegments)
	{
		OutSegments.Reset();
		if (Args.Channels.Num() > 0 && Args.Segments.Num() > 0)
		{
			return false;
		}
		for (const FSuperFAISSChannelWeight& Entry : Args.Channels)
		{
			const int32 ChannelIndex = Bank->GetChannelIndex(Entry.Channel);
			if (ChannelIndex == INDEX_NONE)
			{
				return false;
			}
			const superfaiss::BankView View = Bank->GetBankView();
			superfaiss::QuerySegment Segment;
			Segment.offset = View.channels[ChannelIndex].offset;
			Segment.length = View.channels[ChannelIndex].length;
			Segment.weight = Entry.Weight;
			OutSegments.Add(Segment);
		}
		for (const FSuperFAISSSegment& Raw : Args.Segments)
		{
			superfaiss::QuerySegment Segment;
			Segment.offset = Raw.Offset;
			Segment.length = Raw.Length;
			Segment.weight = Raw.Weight;
			OutSegments.Add(Segment);
		}
		// Core validation requires ascending order; named channels resolve in table
		// order only if the caller listed them so - sort by offset here, once.
		OutSegments.Sort([](const superfaiss::QuerySegment& A,
			const superfaiss::QuerySegment& B) { return A.offset < B.offset; });
		return true;
	}

	// v2.1 per-row bias: resolves Args.RowBias / Args.BiasPairs against a view's
	// row count into core form. Dense length must equal the view count exactly
	// (the N2 alignment contract - on scratch banks the view IS the snapshot);
	// both forms set is rejection. Returns false on malformed input; outBias is
	// null when no bias is present.
	bool ResolveBias(
		int32 ViewCount,
		const FSuperFAISSQueryArgs& Args,
		TArray<superfaiss::BiasPair>& PairScratch,
		superfaiss::RowBias& BiasScratch,
		const superfaiss::RowBias*& OutBias)
	{
		OutBias = nullptr;
		const bool bDense = Args.RowBias.Num() > 0;
		const bool bSparse = Args.BiasPairs.Num() > 0;
		if (!bDense && !bSparse)
		{
			return true;
		}
		if (bDense && bSparse)
		{
			return false;
		}
		BiasScratch = superfaiss::RowBias{};
		if (bDense)
		{
			if (Args.RowBias.Num() != ViewCount)
			{
				return false; // index-aligned or rejected, never silently misaligned
			}
			BiasScratch.dense = Args.RowBias.GetData();
		}
		else
		{
			PairScratch.Reset();
			PairScratch.Reserve(Args.BiasPairs.Num());
			for (const FSuperFAISSBiasPair& Pair : Args.BiasPairs)
			{
				superfaiss::BiasPair Core;
				Core.index = Pair.Index;
				Core.bias = Pair.Bias;
				PairScratch.Add(Core);
			}
			BiasScratch.pairs = PairScratch.GetData();
			BiasScratch.pairCount = PairScratch.Num();
		}
		OutBias = &BiasScratch;
		return true;
	}

	// D-V2-1 query-side rule: on Cosine channel banks, the query's channel
	// sub-vectors renormalize once at build so segment scores are true cosines.
	void RenormalizeQueryChannels(
		const USuperFAISSVectorBank* Bank,
		const TArray<superfaiss::QuerySegment>& Segments,
		float* PaddedQuery)
	{
		if (Bank->Metric != ESuperFAISSBankMetric::Cosine ||
			Bank->GetChannelCount() == 0)
		{
			return;
		}
		for (const superfaiss::QuerySegment& Segment : Segments)
		{
			if (Segment.weight == 0.0f)
			{
				continue;
			}
			double Norm = 0.0;
			for (int32 J = Segment.offset; J < Segment.offset + Segment.length; ++J)
			{
				Norm += static_cast<double>(PaddedQuery[J]) * PaddedQuery[J];
			}
			if (Norm > 0.0)
			{
				const double Inv = 1.0 / FMath::Sqrt(Norm);
				for (int32 J = Segment.offset; J < Segment.offset + Segment.length; ++J)
				{
					PaddedQuery[J] = static_cast<float>(PaddedQuery[J] * Inv);
				}
			}
			// A zero-norm sub-vector stays zero: the core rejects it (W3 query side).
		}
	}

	// Stages an unpadded query into aligned, zero-padded scratch and runs the core
	// query. Shared by the sync, async, and Blueprint paths.
	bool RunQuery(
		const USuperFAISSVectorBank* Bank,
		TConstArrayView<float> UnpaddedQuery,
		const FSuperFAISSQueryArgs& Args,
		USuperFAISSSubsystem::FPooledWorkspace& Scratch,
		TArray<FSuperFAISSHit>& OutHits)
	{
		using namespace superfaiss;

		OutHits.Reset();
		if (Bank == nullptr || !Bank->IsValid() || Args.K <= 0 ||
			UnpaddedQuery.Num() != Bank->Dims)
		{
			return false;
		}
		const BankView View = Bank->GetBankView();
		if (Args.ExcludeBits.Num() != 0 && Args.ExcludeBits.Num() < (View.count + 31) / 32)
		{
			return false;
		}

		if (Scratch.QueryStaging.Num() < View.paddedDims)
		{
			Scratch.QueryStaging.SetNumZeroed(View.paddedDims);
		}
		FMemory::Memzero(Scratch.QueryStaging.GetData(), View.paddedDims * sizeof(float));
		FMemory::Memcpy(Scratch.QueryStaging.GetData(), UnpaddedQuery.GetData(),
			UnpaddedQuery.Num() * sizeof(float));

		if (Scratch.HitStaging.Num() < Args.K)
		{
			Scratch.HitStaging.SetNumUninitialized(Args.K);
		}
		int32_t HitCountForSegments = 0;

		TArray<QuerySegment> Segments;
		if (!ResolveSegments(Bank, Args, Segments))
		{
			return false;
		}
		if (Segments.Num() > 0)
		{
			RenormalizeQueryChannels(Bank, Segments, Scratch.QueryStaging.GetData());
		}

		TArray<BiasPair> PairScratch;
		RowBias BiasScratch;
		const RowBias* Bias = nullptr;
		if (!ResolveBias(View.count, Args, PairScratch, BiasScratch, Bias))
		{
			return false;
		}

		QueryParams Params;
		Params.k = Args.K;
		Params.excludeBits = Args.ExcludeBits.Num() ? Args.ExcludeBits.GetData() : nullptr;
		Params.scoreAs = Args.bScoreAsDot ? ScoreAs::Dot : ScoreAs::BankMetric;
		Params.segments = Segments.Num() > 0 ? Segments.GetData() : nullptr;
		Params.segmentCount = Segments.Num();
		Params.bias = Bias;
		Params.exactness = Args.bCrossDeviceExact
			? Exactness::CrossDevice : Exactness::PerDevice;

		// Effective view for scoring/ordering under the override. Validation always
		// runs against the bank's own view (the core applies the same split), so the
		// serial and parallel paths reject identically.
		BankView Scoring = View;
		Scoring.metric = ScoringMetric(View, Params);

		// Segmented, biased, and cross-device queries route through the core
		// (fold/dense/bias composition and the integer kernels live there); the
		// pooled parallel fan-out below stays a V1-only fast path and is bypassed
		// for all three - serial core scans are already sub-millisecond at game
		// scale.
		if (Params.segments != nullptr || Params.bias != nullptr ||
			Params.exactness == Exactness::CrossDevice)
		{
			const Status SegResult = Query(View, Scratch.QueryStaging.GetData(), Params,
				Scratch.Core, Scratch.HitStaging.GetData(), &HitCountForSegments);
			if (SegResult != Status::Ok)
			{
				return false;
			}
			const int32 N = HitCountForSegments;
			OutHits.SetNum(N);
			const Metric ScoredMetric = Scoring.metric;
			for (int32 i = 0; i < N; ++i)
			{
				OutHits[i].Index = Scratch.HitStaging[i].index;
				OutHits[i].Id = Bank->GetIdForIndex(Scratch.HitStaging[i].index);
				OutHits[i].Score = Scratch.HitStaging[i].score;
				OutHits[i].Margin = (i + 1 < N)
					? Margin(Scratch.HitStaging[i], Scratch.HitStaging[i + 1], ScoredMetric)
					: 0.0f;
			}
			return true;
		}

		// Path selection: parallel scan when configured and the bank is big enough for
		// the fan-out to pay. ParallelFor joins before this function returns, so the
		// workspace single-owner contract and the shutdown drain are unaffected.
		const int32 Chunks = ChunkCount(View);
		const int32 Mode = CVarSuperFAISSParallelScan.GetValueOnAnyThread();
		const bool bParallel = Mode == 2 ||
			(Mode == 1 && Chunks >= CVarSuperFAISSParallelMinChunks.GetValueOnAnyThread());

		int32_t HitCount = 0;
		if (bParallel && Chunks > 1)
		{
			const Status QueryStatus = ValidateQuery(View, Scratch.QueryStaging.GetData());
			if (QueryStatus != Status::Ok)
			{
				return false;
			}

			// Capacities are independent: per-chunk hit storage scales with Chunks*K,
			// the list arrays with Chunks alone (a many-chunks/small-K query must not
			// be satisfied by a large Chunks*K reservation from a previous query).
			const int32 K = Args.K;
			// int64 guard mirrors the batch path (Poirot M6): caller-unbounded K must
			// not wrap the capacity math.
			const int64 SingleCellCount = static_cast<int64>(Chunks) * K;
			if (SingleCellCount > MAX_int32)
			{
				return false;
			}
			if (Scratch.ChunkHeapStorage.Num() < SingleCellCount)
			{
				Scratch.ChunkHeapStorage.SetNumUninitialized(SingleCellCount);
				Scratch.ChunkSorted.SetNumUninitialized(SingleCellCount);
			}
			if (Scratch.ChunkListPtrs.Num() < Chunks)
			{
				Scratch.ChunkListPtrs.SetNumUninitialized(Chunks);
				Scratch.ChunkListCounts.SetNumUninitialized(Chunks);
			}
			if (Scratch.MergeHeap.Num() < K)
			{
				Scratch.MergeHeap.SetNumUninitialized(K);
			}

			superfaiss::Hit* HeapBase = Scratch.ChunkHeapStorage.GetData();
			superfaiss::Hit* SortedBase = Scratch.ChunkSorted.GetData();
			const superfaiss::Hit** ListPtrs = Scratch.ChunkListPtrs.GetData();
			int32_t* ListCounts = Scratch.ChunkListCounts.GetData();
			const uint32_t* Exclude = Params.excludeBits;

			ParallelFor(Chunks, [&Scoring, &Scratch, HeapBase, SortedBase, ListPtrs,
				ListCounts, Exclude, K](int32 Chunk)
			{
				TopK ChunkTopK;
				ChunkTopK.Init(HeapBase + static_cast<int64>(Chunk) * K, K, Scoring.metric);
				ScoreChunk(Scoring, Scratch.QueryStaging.GetData(), Chunk, Exclude, ChunkTopK);
				ListPtrs[Chunk] = SortedBase + static_cast<int64>(Chunk) * K;
				ListCounts[Chunk] = ChunkTopK.Finalize(SortedBase + static_cast<int64>(Chunk) * K);
			});

			HitCount = MergeTopK(ListPtrs, ListCounts, Chunks, Scoring.metric, K,
				Scratch.MergeHeap.GetData(), Scratch.HitStaging.GetData());
		}
		else
		{
			const Status Result = Query(View, Scratch.QueryStaging.GetData(), Params,
				Scratch.Core, Scratch.HitStaging.GetData(), &HitCount);
			if (Result != Status::Ok)
			{
				return false;
			}
		}

		OutHits.SetNum(HitCount);
		for (int32 i = 0; i < HitCount; ++i)
		{
			OutHits[i].Index = Scratch.HitStaging[i].index;
			OutHits[i].Id = Bank->GetIdForIndex(Scratch.HitStaging[i].index);
			OutHits[i].Score = Scratch.HitStaging[i].score;
			OutHits[i].Margin = (i + 1 < HitCount)
				? Margin(Scratch.HitStaging[i], Scratch.HitStaging[i + 1], Scoring.metric)
				: 0.0f;
		}
		return true;
	}
}

TSharedPtr<USuperFAISSSubsystem::FPooledWorkspace>
USuperFAISSSubsystem::AcquireWorkspace()
{
	{
		FScopeLock Lock(&PoolLock);
		if (Pool.Num() > 0)
		{
			return Pool.Pop(EAllowShrinking::No);
		}
	}
	PoolGrowth.fetch_add(1, std::memory_order_relaxed);
	return MakeShared<FPooledWorkspace>();
}

void USuperFAISSSubsystem::ReleaseWorkspace(TSharedPtr<FPooledWorkspace> Workspace)
{
	FScopeLock Lock(&PoolLock);
	Pool.Push(MoveTemp(Workspace));
}

uint64 USuperFAISSSubsystem::GetPoolGrowthCount() const
{
	return PoolGrowth.load(std::memory_order_relaxed);
}

void USuperFAISSSubsystem::Deinitialize()
{
	// Refuse new dispatches, then wait out the fleet. Deliveries are queued to the
	// game thread — which is this thread — so the queue must be pumped while waiting
	// or the counter can never reach zero.
	// The bDraining/InFlightAsync ordering is race-free only because dispatch and
	// drain share the game thread (Poirot O7) — pin the assumption.
	check(IsInGameThread());
	bDraining.store(true);
	while (InFlightAsync.load() > 0)
	{
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		FPlatformProcess::Sleep(0.0005f);
	}
	Super::Deinitialize();
}

bool USuperFAISSSubsystem::QuerySync(
	const USuperFAISSVectorBank* Bank,
	TConstArrayView<float> UnpaddedQuery,
	const FSuperFAISSQueryArgs& Args,
	TArray<FSuperFAISSHit>& OutHits)
{
	SCOPE_CYCLE_COUNTER(STAT_SuperFAISSQuerySync);
	TSharedPtr<FPooledWorkspace> Scratch = AcquireWorkspace();
	const bool bOk = RunQuery(Bank, UnpaddedQuery, Args, *Scratch, OutHits);
	ReleaseWorkspace(MoveTemp(Scratch));
	return bOk;
}

FSuperFAISSTicket USuperFAISSSubsystem::QueryAsync(
	const USuperFAISSVectorBank* Bank,
	TConstArrayView<float> UnpaddedQuery,
	const FSuperFAISSQueryArgs& Args,
	FSuperFAISSNativeResultDelegate Completion)
{
	check(IsInGameThread()); // dispatch must share the drain's thread (Poirot O7)
	FSuperFAISSTicket Ticket;
	Ticket.CancelFlag = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);

	// Shutting down: fail fast without touching pools or pinning anything.
	if (bDraining.load())
	{
		AsyncTask(ENamedThreads::GameThread, [Completion = MoveTemp(Completion)]() mutable
		{
			Completion.ExecuteIfBound(TArray<FSuperFAISSHit>(), false);
		});
		return Ticket;
	}

	// Pin the bank against GC for the task's lifetime; copy the query, the
	// exclusion bits, and the FULL args — the caller's views need not outlive
	// this call, and an async query must run the same query the sync path would
	// (Poirot P-3: the original copy carried only K/ExcludeBits/bScoreAsDot, so
	// async segmented/channel/bias/cross-device queries silently ran plain and
	// reported success). The TArray members deep-copy; ExcludeBits is a view, so
	// it is detached here and re-pointed at the task-owned copy inside the task.
	TStrongObjectPtr<const USuperFAISSVectorBank> PinnedBank(Bank);
	TArray<float> QueryCopy(UnpaddedQuery.GetData(), UnpaddedQuery.Num());
	TArray<uint32> ExcludeCopy(Args.ExcludeBits.GetData(), Args.ExcludeBits.Num());
	FSuperFAISSQueryArgs ArgsCopy = Args;
	ArgsCopy.ExcludeBits = TConstArrayView<uint32>();
	TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> CancelFlag = Ticket.CancelFlag;

	// Counted from dispatch until the game-thread delivery finishes, so the
	// Deinitialize drain covers the whole lifetime that touches `this` or the bank.
	InFlightAsync.fetch_add(1);

	FFunctionGraphTask::CreateAndDispatchWhenReady(
		[this, PinnedBank = MoveTemp(PinnedBank), QueryCopy = MoveTemp(QueryCopy),
			ExcludeCopy = MoveTemp(ExcludeCopy), ArgsCopy = MoveTemp(ArgsCopy),
			CancelFlag, Completion = MoveTemp(Completion)]() mutable
		{
			SCOPE_CYCLE_COUNTER(STAT_SuperFAISSQueryAsync);

			TArray<FSuperFAISSHit> Hits;
			bool bOk = false;
			if (!CancelFlag->load())
			{
				ArgsCopy.ExcludeBits = ExcludeCopy; // task-owned storage, task-local view
				TSharedPtr<FPooledWorkspace> Scratch = AcquireWorkspace();
				bOk = RunQuery(PinnedBank.Get(), QueryCopy, ArgsCopy, *Scratch, Hits);
				ReleaseWorkspace(MoveTemp(Scratch));
			}
			const bool bSuccess = bOk && !CancelFlag->load();

			// Deliver (and release the pin) on the game thread; the in-flight count
			// drops only after delivery, closing the shutdown-purge window.
			AsyncTask(ENamedThreads::GameThread,
				[this, PinnedBank = MoveTemp(PinnedBank), Hits = MoveTemp(Hits), bSuccess,
					Completion = MoveTemp(Completion)]() mutable
				{
					Completion.ExecuteIfBound(Hits, bSuccess);
					PinnedBank.Reset();
					InFlightAsync.fetch_sub(1);
				});
		},
		TStatId(), nullptr, ENamedThreads::AnyBackgroundThreadNormalTask);

	return Ticket;
}

bool USuperFAISSSubsystem::QueryScratch(
	USuperFAISSScratchBank* Bank,
	TConstArrayView<float> UnpaddedQuery,
	const FSuperFAISSQueryArgs& Args,
	TArray<FSuperFAISSHit>& OutHits)
{
	using namespace superfaiss;

	OutHits.Reset();
	// Channels are asset-side vocabulary; scratch banks carry no channel table.
	if (Bank == nullptr || !Bank->IsInitialized() || Args.K <= 0 ||
		Args.Channels.Num() > 0 || UnpaddedQuery.Num() != Bank->GetDims())
	{
		return false;
	}
	// The N4 dispatch gate: a Grow/Freeze/Load waiting on the pin counter refuses
	// new queries here, so a busy consumer cannot starve it.
	if (!Bank->TryPin())
	{
		return false;
	}
	ON_SCOPE_EXIT { Bank->Unpin(); };

	TSharedPtr<FPooledWorkspace> Scratch = AcquireWorkspace();
	bool bOk = false;
	do
	{
		BankView View;
		// Size the staging from CAPACITY, not the current count (Poirot P-1): the
		// pin excludes Grow/Freeze/Load but NOT Append - that concurrency is the
		// scratch bank's designed model - so Snapshot's own count read can exceed
		// a count read here, and a word-boundary crossing in that window would
		// overrun a count-sized buffer. Capacity is pin-stable and bounds every
		// count Snapshot can observe; it is also never zero, which makes the
		// empty-bank query well-defined regardless of pool history (P-2).
		const int32 Words = ScratchBank::TombstoneWords(Bank->Core().Capacity());
		if (Scratch->TombstoneStaging.Num() < Words)
		{
			Scratch->TombstoneStaging.SetNumZeroed(Words);
		}
		if (Bank->Core().Snapshot(&View, Scratch->TombstoneStaging.GetData()) != Status::Ok)
		{
			break;
		}
		// Deletion is exclusion (V2-G4): the snapshot's tombstones OR the caller's own set.
		if (Args.ExcludeBits.Num() != 0)
		{
			if (Args.ExcludeBits.Num() < (View.count + 31) / 32)
			{
				break;
			}
			for (int32 W = 0; W < (View.count + 31) / 32; ++W)
			{
				Scratch->TombstoneStaging[W] |= Args.ExcludeBits[W];
			}
		}

		if (Scratch->QueryStaging.Num() < View.paddedDims)
		{
			Scratch->QueryStaging.SetNumZeroed(View.paddedDims);
		}
		FMemory::Memzero(Scratch->QueryStaging.GetData(), View.paddedDims * sizeof(float));
		FMemory::Memcpy(Scratch->QueryStaging.GetData(), UnpaddedQuery.GetData(),
			UnpaddedQuery.Num() * sizeof(float));
		if (Scratch->HitStaging.Num() < Args.K)
		{
			Scratch->HitStaging.SetNumUninitialized(Args.K);
		}

		TArray<QuerySegment> Segments;
		for (const FSuperFAISSSegment& Raw : Args.Segments)
		{
			QuerySegment Segment;
			Segment.offset = Raw.Offset;
			Segment.length = Raw.Length;
			Segment.weight = Raw.Weight;
			Segments.Add(Segment);
		}
		Segments.Sort([](const QuerySegment& A, const QuerySegment& B) {
			return A.offset < B.offset;
		});

		// v2.1 bias on scratch: index-aligned to THIS snapshot (T-055 N2) - a dense
		// view sized for any other count is rejection, never silent misalignment;
		// the equal-count remove-then-append hazard is forbidden by the pin/drain
		// machinery this query already runs under.
		TArray<BiasPair> PairScratchBias;
		RowBias BiasScratch;
		const RowBias* Bias = nullptr;
		if (!ResolveBias(View.count, Args, PairScratchBias, BiasScratch, Bias))
		{
			break;
		}

		QueryParams Params;
		Params.k = Args.K;
		Params.excludeBits = View.count > 0 ? Scratch->TombstoneStaging.GetData() : nullptr;
		Params.scoreAs = Args.bScoreAsDot ? ScoreAs::Dot : ScoreAs::BankMetric;
		Params.segments = Segments.Num() > 0 ? Segments.GetData() : nullptr;
		Params.segmentCount = Segments.Num();
		Params.bias = Bias;
		Params.exactness = Args.bCrossDeviceExact
			? Exactness::CrossDevice : Exactness::PerDevice;

		int32_t HitCount = 0;
		if (Query(View, Scratch->QueryStaging.GetData(), Params, Scratch->Core,
				Scratch->HitStaging.GetData(), &HitCount) != Status::Ok)
		{
			break;
		}
		const Metric ScoredMetric = ScoringMetric(View, Params);
		OutHits.SetNum(HitCount);
		for (int32 i = 0; i < HitCount; ++i)
		{
			OutHits[i].Index = Scratch->HitStaging[i].index;
			OutHits[i].Id = NAME_None; // scratch rows are index-addressed (W4 contract)
			OutHits[i].Score = Scratch->HitStaging[i].score;
			OutHits[i].Margin = (i + 1 < HitCount)
				? Margin(Scratch->HitStaging[i], Scratch->HitStaging[i + 1], ScoredMetric)
				: 0.0f;
		}
		bOk = true;
	} while (false);
	ReleaseWorkspace(Scratch);
	return bOk;
}

bool USuperFAISSSubsystem::QuerySimilarScratch(USuperFAISSScratchBank* Bank,
	const TArray<float>& Query, int32 K, TArray<FSuperFAISSHit>& Hits)
{
	FSuperFAISSQueryArgs Args;
	Args.K = K;
	return QueryScratch(Bank, Query, Args, Hits);
}

bool USuperFAISSSubsystem::QueryBatch(
	const USuperFAISSVectorBank* Bank,
	TConstArrayView<float> UnpaddedQueries,
	int32 QueryCount,
	const FSuperFAISSQueryArgs& Args,
	TArray<FSuperFAISSHit>& OutHits,
	TArray<int32>& OutCounts,
	TConstArrayView<FSuperFAISSBiasPair> PerQueryBiasPairs)
{
	SCOPE_CYCLE_COUNTER(STAT_SuperFAISSQueryBatch);
	using namespace superfaiss;

	OutHits.Reset();
	OutCounts.Reset();
	if (Bank == nullptr || !Bank->IsValid() || Args.K <= 0 || QueryCount < 0 ||
		UnpaddedQueries.Num() != static_cast<int64>(QueryCount) * Bank->Dims)
	{
		return false;
	}
	if (QueryCount == 0)
	{
		return true;
	}
	const BankView View = Bank->GetBankView();
	if (Args.ExcludeBits.Num() != 0 && Args.ExcludeBits.Num() < (View.count + 31) / 32)
	{
		return false;
	}

	TSharedPtr<FPooledWorkspace> Scratch = AcquireWorkspace();

	// Stage all queries padded + aligned (contiguous, stride paddedDims).
	const int64 StagingCount = static_cast<int64>(QueryCount) * View.paddedDims;
	if (Scratch->QueryStaging.Num() < StagingCount)
	{
		Scratch->QueryStaging.SetNumZeroed(StagingCount);
	}
	FMemory::Memzero(Scratch->QueryStaging.GetData(), StagingCount * sizeof(float));
	for (int32 M = 0; M < QueryCount; ++M)
	{
		FMemory::Memcpy(
			Scratch->QueryStaging.GetData() + static_cast<int64>(M) * View.paddedDims,
			UnpaddedQueries.GetData() + static_cast<int64>(M) * Bank->Dims,
			Bank->Dims * sizeof(float));
	}

	const int64 HitCapacity = static_cast<int64>(QueryCount) * Args.K;
	if (HitCapacity > MAX_int32 || StagingCount > MAX_int32)
	{
		ReleaseWorkspace(MoveTemp(Scratch));
		return false;
	}
	if (Scratch->HitStaging.Num() < HitCapacity)
	{
		Scratch->HitStaging.SetNumUninitialized(HitCapacity);
	}
	if (Scratch->CountStaging.Num() < QueryCount)
	{
		Scratch->CountStaging.SetNumUninitialized(QueryCount);
	}
	int32_t* Counts = Scratch->CountStaging.GetData();
	FMemory::Memzero(Counts, QueryCount * sizeof(int32_t));

	// Composition: segments apply to every query; bias is either the shared
	// Args form (one resolved view, replicated per entry - the core's stated
	// degenerate case) or the per-query sparse pairs.
	TArray<QuerySegment> Segments;
	if (!ResolveSegments(Bank, Args, Segments))
	{
		ReleaseWorkspace(MoveTemp(Scratch));
		return false;
	}
	for (int32 M = 0; Segments.Num() > 0 && M < QueryCount; ++M)
	{
		RenormalizeQueryChannels(Bank, Segments,
			Scratch->QueryStaging.GetData() + static_cast<int64>(M) * View.paddedDims);
	}

	TArray<BiasPair> SharedPairScratch;
	RowBias SharedBiasScratch;
	const RowBias* SharedBias = nullptr;
	if (!ResolveBias(View.count, Args, SharedPairScratch, SharedBiasScratch, SharedBias))
	{
		ReleaseWorkspace(MoveTemp(Scratch));
		return false;
	}
	if (PerQueryBiasPairs.Num() != 0 &&
		(PerQueryBiasPairs.Num() != QueryCount || SharedBias != nullptr))
	{
		ReleaseWorkspace(MoveTemp(Scratch));
		return false;
	}
	TArray<RowBias> BiasEntries;      // per-query core entries
	TArray<BiasPair> PerQueryStorage; // stable pair storage the entries point at
	if (SharedBias != nullptr)
	{
		BiasEntries.Init(*SharedBias, QueryCount);
	}
	else if (PerQueryBiasPairs.Num() == QueryCount)
	{
		BiasEntries.Init(RowBias(), QueryCount);
		PerQueryStorage.SetNumUninitialized(QueryCount);
		bool bAnyPair = false;
		for (int32 M = 0; M < QueryCount; ++M)
		{
			if (PerQueryBiasPairs[M].Index != INDEX_NONE)
			{
				PerQueryStorage[M] = {PerQueryBiasPairs[M].Index,
					PerQueryBiasPairs[M].Bias};
				BiasEntries[M].pairs = &PerQueryStorage[M];
				BiasEntries[M].pairCount = 1;
				bAnyPair = true;
			}
		}
		if (!bAnyPair)
		{
			BiasEntries.Reset(); // all-unbiased: the bit-identical null path
		}
	}

	QueryParams Params;
	Params.k = Args.K;
	Params.excludeBits = Args.ExcludeBits.Num() ? Args.ExcludeBits.GetData() : nullptr;
	Params.scoreAs = Args.bScoreAsDot ? ScoreAs::Dot : ScoreAs::BankMetric;
	Params.segments = Segments.Num() > 0 ? Segments.GetData() : nullptr;
	Params.segmentCount = Segments.Num();
	Params.bias = BiasEntries.Num() > 0 ? BiasEntries.GetData() : nullptr;
	Params.exactness = Args.bCrossDeviceExact
		? Exactness::CrossDevice : Exactness::PerDevice;

	// Effective view for scoring/ordering; validation stays on the bank's view (same
	// split as RunQuery and the core).
	BankView Scoring = View;
	Scoring.metric = ScoringMetric(View, Params);

	// Path selection mirrors RunQuery: chunk-outer ParallelFor when configured and the
	// bank is big enough; each chunk scores every query (pair kernels) while its rows
	// are cache-resident, then each query's per-chunk lists merge deterministically.
	// Results are bit-identical to the serial path (total order + pair ≡ single bits).
	const int32 Chunks = ChunkCount(View);
	const int32 Mode = CVarSuperFAISSParallelScan.GetValueOnAnyThread();
	// Composed batches (segments, bias, cross-device) take the serial core batch,
	// like segments and bias in the single-query path.
	const bool bComposed = Params.segments != nullptr || Params.bias != nullptr ||
		Args.bCrossDeviceExact;
	const bool bParallel = !bComposed && (Mode == 2 ||
		(Mode == 1 && Chunks >= CVarSuperFAISSParallelMinChunks.GetValueOnAnyThread()));

	if (bParallel && Chunks > 1)
	{
		for (int32 M = 0; M < QueryCount; ++M)
		{
			const Status QueryStatus = ValidateQuery(View,
				Scratch->QueryStaging.GetData() + static_cast<int64>(M) * View.paddedDims);
			if (QueryStatus != Status::Ok)
			{
				ReleaseWorkspace(MoveTemp(Scratch));
				return false;
			}
		}

		const int32 K = Args.K;
		const int64 CellCount = static_cast<int64>(Chunks) * QueryCount * K;
		if (CellCount > MAX_int32)
		{
			ReleaseWorkspace(MoveTemp(Scratch));
			return false;
		}
		if (Scratch->ChunkHeapStorage.Num() < CellCount)
		{
			Scratch->ChunkHeapStorage.SetNumUninitialized(CellCount);
			Scratch->ChunkSorted.SetNumUninitialized(CellCount);
		}
		if (Scratch->ChunkListCounts.Num() < Chunks * QueryCount)
		{
			Scratch->ChunkListCounts.SetNumUninitialized(Chunks * QueryCount);
		}
		if (Scratch->ChunkListPtrs.Num() < Chunks)
		{
			Scratch->ChunkListPtrs.SetNumUninitialized(Chunks);
		}
		if (Scratch->MergeHeap.Num() < K)
		{
			Scratch->MergeHeap.SetNumUninitialized(K);
		}

		superfaiss::Hit* HeapBase = Scratch->ChunkHeapStorage.GetData();
		superfaiss::Hit* SortedBase = Scratch->ChunkSorted.GetData();
		int32_t* CellCounts = Scratch->ChunkListCounts.GetData();
		const float* QueryBase = Scratch->QueryStaging.GetData();
		const uint32_t* Exclude = Params.excludeBits;
		const int32 Pd = View.paddedDims;

		ParallelFor(Chunks, [&Scoring, HeapBase, SortedBase, CellCounts, QueryBase, Exclude,
			K, QueryCount, Pd](int32 Chunk)
		{
			TopK PairA;
			TopK PairB;
			int32 M = 0;
			for (; M + 2 <= QueryCount; M += 2)
			{
				const int64 CellA = static_cast<int64>(Chunk) * QueryCount + M;
				PairA.Init(HeapBase + CellA * K, K, Scoring.metric);
				PairB.Init(HeapBase + (CellA + 1) * K, K, Scoring.metric);
				ScoreChunkPair(Scoring,
					QueryBase + static_cast<int64>(M) * Pd,
					QueryBase + static_cast<int64>(M + 1) * Pd,
					Chunk, Exclude, PairA, PairB);
				CellCounts[CellA] = PairA.Finalize(SortedBase + CellA * K);
				CellCounts[CellA + 1] = PairB.Finalize(SortedBase + (CellA + 1) * K);
			}
			if (M < QueryCount)
			{
				const int64 Cell = static_cast<int64>(Chunk) * QueryCount + M;
				PairA.Init(HeapBase + Cell * K, K, Scoring.metric);
				ScoreChunk(Scoring, QueryBase + static_cast<int64>(M) * Pd, Chunk, Exclude, PairA);
				CellCounts[Cell] = PairA.Finalize(SortedBase + Cell * K);
			}
		});

		// Per-query deterministic merges. MergeCountScratch is pooled (allocation-free
		// once warm); list order is irrelevant by total order, chunk order used anyway.
		if (Scratch->MergeCountScratch.Num() < Chunks)
		{
			Scratch->MergeCountScratch.SetNumUninitialized(Chunks);
		}
		const superfaiss::Hit** ListPtrs = Scratch->ChunkListPtrs.GetData();
		int32_t* PerChunkCounts = Scratch->MergeCountScratch.GetData();
		for (int32 M = 0; M < QueryCount; ++M)
		{
			for (int32 Chunk = 0; Chunk < Chunks; ++Chunk)
			{
				const int64 Cell = static_cast<int64>(Chunk) * QueryCount + M;
				ListPtrs[Chunk] = SortedBase + Cell * K;
				PerChunkCounts[Chunk] = CellCounts[Cell];
			}
			Counts[M] = MergeTopK(ListPtrs, PerChunkCounts, Chunks, Scoring.metric,
				K, Scratch->MergeHeap.GetData(),
				Scratch->HitStaging.GetData() + static_cast<int64>(M) * K);
		}
	}
	else
	{
		const Status Result = superfaiss::QueryBatch(View, Scratch->QueryStaging.GetData(),
			QueryCount, Params, Scratch->Core, Scratch->HitStaging.GetData(), Counts);
		if (Result != Status::Ok)
		{
			ReleaseWorkspace(MoveTemp(Scratch));
			return false;
		}
	}

	OutCounts.SetNum(QueryCount);
	OutHits.SetNum(HitCapacity);
	for (int32 M = 0; M < QueryCount; ++M)
	{
		OutCounts[M] = Counts[M];
		for (int32 i = 0; i < Counts[M]; ++i)
		{
			const superfaiss::Hit& H = Scratch->HitStaging[static_cast<int64>(M) * Args.K + i];
			FSuperFAISSHit& Out = OutHits[static_cast<int64>(M) * Args.K + i];
			Out.Index = H.index;
			Out.Id = Bank->GetIdForIndex(H.index);
			Out.Score = H.score;
			Out.Margin = (i + 1 < Counts[M])
				? Margin(H, Scratch->HitStaging[static_cast<int64>(M) * Args.K + i + 1],
					Scoring.metric)
				: 0.0f;
		}
	}
	ReleaseWorkspace(MoveTemp(Scratch));
	return true;
}

bool USuperFAISSSubsystem::QueryIntersect(
	const USuperFAISSVectorBank* Bank,
	TConstArrayView<float> UnpaddedQueries,
	int32 QueryCount,
	const FSuperFAISSQueryArgs& Args,
	TArray<FSuperFAISSHit>& OutHits)
{
	SCOPE_CYCLE_COUNTER(STAT_SuperFAISSQueryIntersect);
	using namespace superfaiss;

	OutHits.Reset();
	if (Bank == nullptr || !Bank->IsValid() || Args.K <= 0 || QueryCount <= 0 ||
		UnpaddedQueries.Num() != static_cast<int64>(QueryCount) * Bank->Dims)
	{
		return false;
	}
	const BankView View = Bank->GetBankView();
	if (Args.ExcludeBits.Num() != 0 && Args.ExcludeBits.Num() < (View.count + 31) / 32)
	{
		return false;
	}

	TSharedPtr<FPooledWorkspace> Scratch = AcquireWorkspace();

	// Stage all member queries padded + aligned, batch-style.
	const int64 StagingCount = static_cast<int64>(QueryCount) * View.paddedDims;
	if (StagingCount > MAX_int32)
	{
		ReleaseWorkspace(MoveTemp(Scratch));
		return false;
	}
	if (Scratch->QueryStaging.Num() < StagingCount)
	{
		Scratch->QueryStaging.SetNumZeroed(StagingCount);
	}
	FMemory::Memzero(Scratch->QueryStaging.GetData(), StagingCount * sizeof(float));
	for (int32 M = 0; M < QueryCount; ++M)
	{
		FMemory::Memcpy(
			Scratch->QueryStaging.GetData() + static_cast<int64>(M) * View.paddedDims,
			UnpaddedQueries.GetData() + static_cast<int64>(M) * Bank->Dims,
			Bank->Dims * sizeof(float));
	}

	if (Scratch->HitStaging.Num() < Args.K)
	{
		Scratch->HitStaging.SetNumUninitialized(Args.K);
	}

	QueryParams Params;
	Params.k = Args.K;
	Params.excludeBits = Args.ExcludeBits.Num() ? Args.ExcludeBits.GetData() : nullptr;
	Params.scoreAs = Args.bScoreAsDot ? ScoreAs::Dot : ScoreAs::BankMetric;
	Params.exactness = Args.bCrossDeviceExact
		? Exactness::CrossDevice : Exactness::PerDevice;

	BankView Scoring = View;
	Scoring.metric = ScoringMetric(View, Params);

	const int32 Chunks = ChunkCount(View);
	const int32 Mode = CVarSuperFAISSParallelScan.GetValueOnAnyThread();
	// Cross-device queries take the serial core path (the integer kernels and
	// their epilogue live there), like segments and bias in the single-query path.
	const bool bParallel = !Args.bCrossDeviceExact && (Mode == 2 ||
		(Mode == 1 && Chunks >= CVarSuperFAISSParallelMinChunks.GetValueOnAnyThread()));

	int32_t HitCount = 0;
	if (bParallel && Chunks > 1)
	{
		for (int32 M = 0; M < QueryCount; ++M)
		{
			const Status QueryStatus = ValidateQuery(View,
				Scratch->QueryStaging.GetData() + static_cast<int64>(M) * View.paddedDims);
			if (QueryStatus != Status::Ok)
			{
				ReleaseWorkspace(MoveTemp(Scratch));
				return false;
			}
		}

		const int32 K = Args.K;
		const int64 SingleCellCount = static_cast<int64>(Chunks) * K;
		if (SingleCellCount > MAX_int32)
		{
			ReleaseWorkspace(MoveTemp(Scratch));
			return false;
		}
		if (Scratch->ChunkHeapStorage.Num() < SingleCellCount)
		{
			Scratch->ChunkHeapStorage.SetNumUninitialized(SingleCellCount);
			Scratch->ChunkSorted.SetNumUninitialized(SingleCellCount);
		}
		if (Scratch->ChunkListPtrs.Num() < Chunks)
		{
			Scratch->ChunkListPtrs.SetNumUninitialized(Chunks);
			Scratch->ChunkListCounts.SetNumUninitialized(Chunks);
		}
		if (Scratch->MergeHeap.Num() < K)
		{
			Scratch->MergeHeap.SetNumUninitialized(K);
		}

		superfaiss::Hit* HeapBase = Scratch->ChunkHeapStorage.GetData();
		superfaiss::Hit* SortedBase = Scratch->ChunkSorted.GetData();
		const superfaiss::Hit** ListPtrs = Scratch->ChunkListPtrs.GetData();
		int32_t* ListCounts = Scratch->ChunkListCounts.GetData();
		const float* QueryBase = Scratch->QueryStaging.GetData();
		const uint32_t* Exclude = Params.excludeBits;

		ParallelFor(Chunks, [&Scoring, HeapBase, SortedBase, ListPtrs, ListCounts,
			QueryBase, Exclude, K, QueryCount](int32 Chunk)
		{
			TopK ChunkTopK;
			ChunkTopK.Init(HeapBase + static_cast<int64>(Chunk) * K, K, Scoring.metric);
			ScoreChunkFused(Scoring, QueryBase, QueryCount, Chunk, Exclude, ChunkTopK);
			ListPtrs[Chunk] = SortedBase + static_cast<int64>(Chunk) * K;
			ListCounts[Chunk] = ChunkTopK.Finalize(SortedBase + static_cast<int64>(Chunk) * K);
		});

		HitCount = MergeTopK(ListPtrs, ListCounts, Chunks, Scoring.metric, K,
			Scratch->MergeHeap.GetData(), Scratch->HitStaging.GetData());
	}
	else
	{
		const Status Result = superfaiss::QueryIntersect(View,
			Scratch->QueryStaging.GetData(), QueryCount, Params, Scratch->Core,
			Scratch->HitStaging.GetData(), &HitCount);
		if (Result != Status::Ok)
		{
			ReleaseWorkspace(MoveTemp(Scratch));
			return false;
		}
	}

	OutHits.SetNum(HitCount);
	for (int32 i = 0; i < HitCount; ++i)
	{
		OutHits[i].Index = Scratch->HitStaging[i].index;
		OutHits[i].Id = Bank->GetIdForIndex(Scratch->HitStaging[i].index);
		OutHits[i].Score = Scratch->HitStaging[i].score;
		OutHits[i].Margin = (i + 1 < HitCount)
			? Margin(Scratch->HitStaging[i], Scratch->HitStaging[i + 1], Scoring.metric)
			: 0.0f;
	}
	ReleaseWorkspace(MoveTemp(Scratch));
	return true;
}

bool USuperFAISSSubsystem::QuerySimilarIntersect(const USuperFAISSVectorBank* Bank,
	const TArray<float>& QueryA, const TArray<float>& QueryB, int32 K,
	TArray<FSuperFAISSHit>& Hits)
{
	Hits.Reset();
	if (QueryA.Num() == 0 || QueryA.Num() != QueryB.Num())
	{
		return false;
	}
	TArray<float> Concatenated;
	Concatenated.Reserve(QueryA.Num() * 2);
	Concatenated.Append(QueryA);
	Concatenated.Append(QueryB);
	FSuperFAISSQueryArgs Args;
	Args.K = K;
	return QueryIntersect(Bank, Concatenated, 2, Args, Hits);
}

bool USuperFAISSSubsystem::QuerySimilarChannels(const USuperFAISSVectorBank* Bank,
	const TArray<float>& Query, const TArray<FSuperFAISSChannelWeight>& Channels,
	int32 K, TArray<FSuperFAISSHit>& Hits)
{
	FSuperFAISSQueryArgs Args;
	Args.K = K;
	Args.Channels = Channels;
	return QuerySync(Bank, Query, Args, Hits);
}

bool USuperFAISSSubsystem::DecomposeHit(const USuperFAISSVectorBank* Bank,
	const TArray<float>& Query, const TArray<FSuperFAISSChannelWeight>& Channels,
	int32 RowIndex, TArray<float>& OutContributions, float& OutTotal, float RowBias,
	bool bCrossDeviceExact)
{
	using namespace superfaiss;

	OutContributions.Reset();
	OutTotal = 0.0f;
	if (Bank == nullptr || !Bank->IsValid() || RowIndex < 0 || RowIndex >= Bank->Count ||
		Query.Num() != Bank->Dims || Channels.Num() == 0)
	{
		return false;
	}

	FSuperFAISSQueryArgs Args;
	Args.Channels = Channels;
	TArray<QuerySegment> Segments;
	if (!ResolveSegments(Bank, Args, Segments))
	{
		return false;
	}

	// Stage padded + renormalized exactly as the scan does, so OutTotal is the
	// scan's own score for this row, bitwise.
	const BankView View = Bank->GetBankView();
	TArray<float, TAlignedHeapAllocator<16>> Staged;
	Staged.SetNumZeroed(View.paddedDims);
	FMemory::Memcpy(Staged.GetData(), Query.GetData(), Query.Num() * sizeof(float));
	RenormalizeQueryChannels(Bank, Segments, Staged.GetData());

	if (ValidateQuery(View, Staged.GetData()) != Status::Ok ||
		ValidateSegments(View, Staged.GetData(), Segments.GetData(),
			Segments.Num()) != Status::Ok)
	{
		return false;
	}

	OutContributions.SetNumZeroed(Segments.Num());
	if (bCrossDeviceExact)
	{
		// Cross-device decomposition (v2.2): quantize exactly as the scan does and
		// run the integer-kernel decomposition, so OutTotal matches a
		// bCrossDeviceExact scan's score for this row bitwise. Int8 banks only,
		// same stride ceiling as the query path.
		if (View.quant != Quantization::Int8 ||
			View.paddedDims > kMaxCrossDeviceDims)
		{
			OutContributions.Reset();
			return false;
		}
		TArray<int8, TAlignedHeapAllocator<16>> Q8;
		Q8.SetNumUninitialized(View.paddedDims);
		XdQuery Xd;
		QuantizeQueryXd(Staged.GetData(), View.paddedDims,
			reinterpret_cast<int8_t*>(Q8.GetData()), &Xd.scale, &Xd.sqSum);
		Xd.q8 = reinterpret_cast<const int8_t*>(Q8.GetData());
		OutTotal = DecomposeRowScoreXd(View, Xd, RowIndex, Segments.GetData(),
			Segments.Num(), OutContributions.GetData());
		// Cross-device bias composes on the floored unbiased float - the same
		// expression the scan executes (guarded like the default path: a zero
		// RowBias arg means "unbiased", which is the no-add path bitwise).
		if (RowBias != 0.0f)
		{
			OutTotal = detail::XdComposeBiasValue(OutTotal, RowBias);
		}
		return true;
	}
	OutTotal = DecomposeRowScore(View, Staged.GetData(), RowIndex, Segments.GetData(),
		Segments.Num(), OutContributions.GetData());
	// The bias term (v2.1) reports separately and composes with the same single
	// add the scan executed. Guarded: an unconditional +0.0 would flip a -0.0
	// total to +0.0 and break the unbiased bitwise contract (the N1 edge). A
	// dense zero-bias row is therefore compare-equal here, exactly as N1 already
	// states for zero bias everywhere.
	if (RowBias != 0.0f)
	{
		OutTotal = OutTotal + RowBias;
	}
	return true;
}

bool USuperFAISSSubsystem::QuerySimilarSync(const USuperFAISSVectorBank* Bank,
	const TArray<float>& Query, int32 K, TArray<FSuperFAISSHit>& Hits)
{
	FSuperFAISSQueryArgs Args;
	Args.K = K;
	return QuerySync(Bank, Query, Args, Hits);
}

bool USuperFAISSSubsystem::QuerySimilarCrossDevice(const USuperFAISSVectorBank* Bank,
	const TArray<float>& Query, int32 K, TArray<FSuperFAISSHit>& Hits)
{
	FSuperFAISSQueryArgs Args;
	Args.K = K;
	Args.bCrossDeviceExact = true;
	return QuerySync(Bank, Query, Args, Hits);
}

void USuperFAISSSubsystem::QuerySimilarAsync(const USuperFAISSVectorBank* Bank,
	const TArray<float>& Query, int32 K, FSuperFAISSResultDelegate OnComplete)
{
	FSuperFAISSQueryArgs Args;
	Args.K = K;
	FSuperFAISSNativeResultDelegate Native;
	Native.BindLambda([OnComplete](const TArray<FSuperFAISSHit>& Hits, bool bSuccess)
	{
		OnComplete.ExecuteIfBound(Hits, bSuccess);
	});
	QueryAsync(Bank, Query, Args, MoveTemp(Native));
}

bool USuperFAISSSubsystem::MakeCentroidQuery(const USuperFAISSVectorBank* Bank,
	const TArray<int32>& RowIndices, TArray<float>& OutQuery)
{
	using namespace superfaiss;

	OutQuery.Reset();
	if (Bank == nullptr || !Bank->IsValid() || RowIndices.Num() == 0)
	{
		return false;
	}
	const BankView View = Bank->GetBankView();

	// The core writes a padded, aligned query; stage through the pooled scratch and
	// hand back the unpadded Dims prefix, matching the query-surface convention.
	TSharedPtr<FPooledWorkspace> Scratch = AcquireWorkspace();
	if (Scratch->QueryStaging.Num() < View.paddedDims)
	{
		Scratch->QueryStaging.SetNumZeroed(View.paddedDims);
	}
	const Status Result = MakeCentroid(
		View, RowIndices.GetData(), RowIndices.Num(), Scratch->QueryStaging.GetData());
	const bool bOk = Result == Status::Ok;
	if (bOk)
	{
		OutQuery.SetNumUninitialized(View.dims);
		FMemory::Memcpy(OutQuery.GetData(), Scratch->QueryStaging.GetData(),
			View.dims * sizeof(float));
	}
	ReleaseWorkspace(MoveTemp(Scratch));
	return bOk;
}

bool USuperFAISSSubsystem::MakeCentroidQueryCrossDevice(const USuperFAISSVectorBank* Bank,
	const TArray<int32>& RowIndices, const TArray<int32>& Weights,
	FSuperFAISSCrossDeviceQuery& OutQuery)
{
	using namespace superfaiss;

	OutQuery = FSuperFAISSCrossDeviceQuery{};
	if (Bank == nullptr || !Bank->IsValid() || RowIndices.Num() == 0)
	{
		return false;
	}
	if (Weights.Num() != 0 && Weights.Num() != RowIndices.Num())
	{
		return false; // one weight per row or none, never silently misaligned
	}
	const BankView View = Bank->GetBankView();

	// The core writes the quantized image into aligned staging; the payload struct
	// then carries a copy of the exact bytes (plus the double scale — no float
	// round-trip — and the integer self-dot).
	TSharedPtr<FPooledWorkspace> Scratch = AcquireWorkspace();
	if (Scratch->XdImageStaging.Num() < View.paddedDims)
	{
		Scratch->XdImageStaging.SetNumZeroed(View.paddedDims);
	}
	double Scale = 0.0;
	int64 SqSum = 0;
	const Status Result = MakeCentroidCrossDevice(View, RowIndices.GetData(),
		RowIndices.Num(), Weights.Num() > 0 ? Weights.GetData() : nullptr, nullptr,
		Scratch->XdImageStaging.GetData(), &Scale, &SqSum);
	const bool bOk = Result == Status::Ok;
	if (bOk)
	{
		OutQuery.ImageQ8.SetNumUninitialized(View.paddedDims);
		FMemory::Memcpy(OutQuery.ImageQ8.GetData(), Scratch->XdImageStaging.GetData(),
			View.paddedDims);
		OutQuery.Scale = Scale;
		OutQuery.SqSum = SqSum;
		OutQuery.Dims = View.dims;
		OutQuery.PaddedDims = View.paddedDims;
	}
	ReleaseWorkspace(MoveTemp(Scratch));
	return bOk;
}

bool USuperFAISSSubsystem::QueryPooledCrossDevice(const USuperFAISSVectorBank* Bank,
	const FSuperFAISSCrossDeviceQuery& Query, int32 K, TArray<FSuperFAISSHit>& Hits)
{
	using namespace superfaiss;

	Hits.Reset();
	if (Bank == nullptr || !Bank->IsValid() || K <= 0 || !Query.IsPayloadValid())
	{
		return false;
	}
	const BankView View = Bank->GetBankView();
	if (Query.Dims != View.dims || Query.PaddedDims != View.paddedDims)
	{
		return false; // the payload is bound to its bank shape
	}

	TSharedPtr<FPooledWorkspace> Scratch = AcquireWorkspace();
	bool bOk = false;
	do
	{
		// The payload's bytes, restaged aligned: the executed image is the caller's
		// image bit-for-bit (a copy preserves bytes; the core never requantizes).
		if (Scratch->XdImageStaging.Num() < View.paddedDims)
		{
			Scratch->XdImageStaging.SetNumZeroed(View.paddedDims);
		}
		FMemory::Memcpy(Scratch->XdImageStaging.GetData(), Query.ImageQ8.GetData(),
			View.paddedDims);
		XdQuery Xd;
		Xd.q8 = Scratch->XdImageStaging.GetData();
		Xd.scale = Query.Scale;
		Xd.sqSum = Query.SqSum;

		if (Scratch->HitStaging.Num() < K)
		{
			Scratch->HitStaging.SetNumUninitialized(K);
		}
		QueryParams Params;
		Params.k = K;
		Params.exactness = Exactness::CrossDevice;
		int32_t HitCount = 0;
		if (QueryXd(View, Xd, Params, Scratch->Core, Scratch->HitStaging.GetData(),
				&HitCount) != Status::Ok)
		{
			break;
		}
		Hits.SetNum(HitCount);
		for (int32 i = 0; i < HitCount; ++i)
		{
			Hits[i].Index = Scratch->HitStaging[i].index;
			Hits[i].Id = Bank->GetIdForIndex(Scratch->HitStaging[i].index);
			Hits[i].Score = Scratch->HitStaging[i].score;
			Hits[i].Margin = (i + 1 < HitCount)
				? Margin(Scratch->HitStaging[i], Scratch->HitStaging[i + 1], View.metric)
				: 0.0f;
		}
		bOk = true;
	} while (false);
	ReleaseWorkspace(MoveTemp(Scratch));
	return bOk;
}

bool USuperFAISSSubsystem::MakeDirectionQuery(const TArray<float>& A,
	const TArray<float>& B, TArray<float>& OutQuery)
{
	using namespace superfaiss;

	OutQuery.Reset();
	if (A.Num() == 0 || A.Num() != B.Num())
	{
		return false;
	}

	// Pure math; the core requires an aligned output buffer.
	TArray<float, TAlignedHeapAllocator<16>> Aligned;
	Aligned.SetNumUninitialized(A.Num());
	const Status Result = MakeDirection(
		A.GetData(), B.GetData(), A.Num(), A.Num(), Aligned.GetData());
	if (Result != Status::Ok)
	{
		return false;
	}
	OutQuery.Append(Aligned.GetData(), A.Num());
	return true;
}

// --- V2.5 bank analytics (plan section 22) ---

bool USuperFAISSSubsystem::SetToSetDistanceCrossDevice(const USuperFAISSVectorBank* BankA,
	const TArray<int32>& RowIndicesA, const TArray<int32>& WeightsA,
	const USuperFAISSVectorBank* BankB, const TArray<int32>& RowIndicesB,
	const TArray<int32>& WeightsB, ESuperFAISSBankMetric Metric, float& OutDistance)
{
	using namespace superfaiss;

	OutDistance = 0.0f;
	if (BankA == nullptr || !BankA->IsValid() || BankB == nullptr || !BankB->IsValid() ||
		RowIndicesA.Num() == 0 || RowIndicesB.Num() == 0)
	{
		return false;
	}
	if ((WeightsA.Num() != 0 && WeightsA.Num() != RowIndicesA.Num()) ||
		(WeightsB.Num() != 0 && WeightsB.Num() != RowIndicesB.Num()))
	{
		return false; // one weight per row or none, never silently misaligned
	}
	const BankView ViewA = BankA->GetBankView();
	const BankView ViewB = BankB->GetBankView();

	TSharedPtr<FPooledWorkspace> Scratch = AcquireWorkspace();
	if (Scratch->XdImageStaging.Num() < ViewA.paddedDims)
	{
		Scratch->XdImageStaging.SetNumZeroed(ViewA.paddedDims);
	}
	if (Scratch->XdImageStagingB.Num() < ViewB.paddedDims)
	{
		Scratch->XdImageStagingB.SetNumZeroed(ViewB.paddedDims);
	}
	const Status Result = CentroidDistanceCrossDevice(
		ViewA, RowIndicesA.GetData(), RowIndicesA.Num(),
		WeightsA.Num() > 0 ? WeightsA.GetData() : nullptr, nullptr,
		ViewB, RowIndicesB.GetData(), RowIndicesB.Num(),
		WeightsB.Num() > 0 ? WeightsB.GetData() : nullptr, nullptr,
		static_cast<superfaiss::Metric>(Metric), Scratch->XdImageStaging.GetData(),
		Scratch->XdImageStagingB.GetData(), &OutDistance);
	ReleaseWorkspace(MoveTemp(Scratch));
	return Result == Status::Ok;
}

bool USuperFAISSSubsystem::MeanNearestNeighborCrossDevice(
	const USuperFAISSVectorBank* SourceBank, const USuperFAISSVectorBank* TargetBank,
	float& OutValue)
{
	using namespace superfaiss;
	OutValue = 0.0f;
	if (SourceBank == nullptr || !SourceBank->IsValid() ||
		TargetBank == nullptr || !TargetBank->IsValid())
	{
		return false;
	}
	const BankView Source = SourceBank->GetBankView();
	const BankView Target = TargetBank->GetBankView();

	TSharedPtr<FPooledWorkspace> Scratch = AcquireWorkspace();
	if (Scratch->XdQueryStaging.Num() < Source.count)
	{
		Scratch->XdQueryStaging.SetNumUninitialized(Source.count);
	}
	if (Scratch->HitStaging.Num() < Source.count)
	{
		Scratch->HitStaging.SetNumUninitialized(Source.count);
	}
	if (Scratch->CountStaging.Num() < Source.count)
	{
		Scratch->CountStaging.SetNumUninitialized(Source.count);
	}
	const Status Result = MeanNNCrossDevice(Source, nullptr, Target, nullptr,
		Scratch->XdQueryStaging.GetData(), Scratch->HitStaging.GetData(),
		Scratch->CountStaging.GetData(), Scratch->Core, &OutValue);
	ReleaseWorkspace(MoveTemp(Scratch));
	return Result == Status::Ok;
}

bool USuperFAISSSubsystem::MaxNearestNeighborCrossDevice(
	const USuperFAISSVectorBank* SourceBank, const USuperFAISSVectorBank* TargetBank,
	float& OutValue)
{
	using namespace superfaiss;
	OutValue = 0.0f;
	if (SourceBank == nullptr || !SourceBank->IsValid() ||
		TargetBank == nullptr || !TargetBank->IsValid())
	{
		return false;
	}
	const BankView Source = SourceBank->GetBankView();
	const BankView Target = TargetBank->GetBankView();

	TSharedPtr<FPooledWorkspace> Scratch = AcquireWorkspace();
	if (Scratch->XdQueryStaging.Num() < Source.count)
	{
		Scratch->XdQueryStaging.SetNumUninitialized(Source.count);
	}
	if (Scratch->HitStaging.Num() < Source.count)
	{
		Scratch->HitStaging.SetNumUninitialized(Source.count);
	}
	if (Scratch->CountStaging.Num() < Source.count)
	{
		Scratch->CountStaging.SetNumUninitialized(Source.count);
	}
	const Status Result = MaxNNCrossDevice(Source, nullptr, Target, nullptr,
		Scratch->XdQueryStaging.GetData(), Scratch->HitStaging.GetData(),
		Scratch->CountStaging.GetData(), Scratch->Core, &OutValue);
	ReleaseWorkspace(MoveTemp(Scratch));
	return Result == Status::Ok;
}

bool USuperFAISSSubsystem::BankSpreadCrossDevice(const USuperFAISSVectorBank* Bank,
	const TArray<int32>& RowIndices, ESuperFAISSReduce Reduce, float& OutValue)
{
	using namespace superfaiss;
	OutValue = 0.0f;
	if (Bank == nullptr || !Bank->IsValid() || RowIndices.Num() == 0)
	{
		return false;
	}
	const BankView View = Bank->GetBankView();

	TSharedPtr<FPooledWorkspace> Scratch = AcquireWorkspace();
	if (Scratch->XdImageStaging.Num() < View.paddedDims)
	{
		Scratch->XdImageStaging.SetNumZeroed(View.paddedDims);
	}
	const Status Result = SpreadCrossDevice(View, RowIndices.GetData(), RowIndices.Num(),
		nullptr, static_cast<superfaiss::Reduce>(Reduce), Scratch->XdImageStaging.GetData(),
		&OutValue);
	ReleaseWorkspace(MoveTemp(Scratch));
	return Result == Status::Ok;
}

bool USuperFAISSSubsystem::ScoreCrossDeviceQueryPair(const FSuperFAISSCrossDeviceQuery& A,
	const FSuperFAISSCrossDeviceQuery& B, ESuperFAISSBankMetric Metric, float& OutScore)
{
	using namespace superfaiss;
	OutScore = 0.0f;
	// The plugin trust-boundary mirror of the core payload law (both payloads
	// self-consistent). The D-V2-13 -128 guard lives in the core ScoreXdPair below —
	// a -128 image with a correct self-dot passes IsPayloadValid and is rejected there,
	// which is the behaviour U1 asserts.
	if (!A.IsPayloadValid() || !B.IsPayloadValid() || A.PaddedDims != B.PaddedDims)
	{
		return false;
	}

	TSharedPtr<FPooledWorkspace> Scratch = AcquireWorkspace();
	if (Scratch->XdImageStaging.Num() < A.PaddedDims)
	{
		Scratch->XdImageStaging.SetNumZeroed(A.PaddedDims);
	}
	if (Scratch->XdImageStagingB.Num() < B.PaddedDims)
	{
		Scratch->XdImageStagingB.SetNumZeroed(B.PaddedDims);
	}
	// Restage both images aligned (the payload's TArray<uint8> carries no alignment).
	FMemory::Memcpy(Scratch->XdImageStaging.GetData(), A.ImageQ8.GetData(), A.PaddedDims);
	FMemory::Memcpy(Scratch->XdImageStagingB.GetData(), B.ImageQ8.GetData(), B.PaddedDims);
	XdQuery Xa;
	Xa.q8 = Scratch->XdImageStaging.GetData();
	Xa.scale = A.Scale;
	Xa.sqSum = A.SqSum;
	XdQuery Xb;
	Xb.q8 = Scratch->XdImageStagingB.GetData();
	Xb.scale = B.Scale;
	Xb.sqSum = B.SqSum;
	const Status Result = ScoreXdPair(Xa, Xb, A.PaddedDims, static_cast<superfaiss::Metric>(Metric),
		&OutScore);
	ReleaseWorkspace(MoveTemp(Scratch));
	return Result == Status::Ok;
}

bool USuperFAISSSubsystem::ProjectionReport(const USuperFAISSVectorBank* Bank,
	const TArray<float>& DirectionA, const TArray<float>& DirectionB,
	const TArray<int32>& GroupA, TArray<float>& OutProjections, float& OutSeparation)
{
	using namespace superfaiss;
	OutProjections.Reset();
	OutSeparation = 0.0f;
	if (Bank == nullptr || !Bank->IsValid())
	{
		return false;
	}
	const BankView View = Bank->GetBankView();
	if (DirectionA.Num() != View.dims || DirectionB.Num() != View.dims)
	{
		return false;
	}

	// Build the unit probe direction on the padded grid: normalize(A - B), padded with
	// zeros (row padding dims are zero, so the padded direction dots the real components
	// only). Offline authoring tooling — local aligned buffers, not the warm pool.
	using FAlignedFloats = TArray<float, TAlignedHeapAllocator<16>>;
	FAlignedFloats PaddedA;
	FAlignedFloats PaddedB;
	FAlignedFloats PaddedDir;
	PaddedA.SetNumZeroed(View.paddedDims);
	PaddedB.SetNumZeroed(View.paddedDims);
	PaddedDir.SetNumZeroed(View.paddedDims);
	FMemory::Memcpy(PaddedA.GetData(), DirectionA.GetData(), View.dims * sizeof(float));
	FMemory::Memcpy(PaddedB.GetData(), DirectionB.GetData(), View.dims * sizeof(float));
	if (MakeDirection(PaddedA.GetData(), PaddedB.GetData(), View.dims, View.paddedDims,
			PaddedDir.GetData()) != Status::Ok)
	{
		return false;
	}

	// Group A = the set bits over all rows; the complement is group B (the core rejects
	// an empty complement when a separation is requested — the F3 no-partial contract).
	TArray<uint32> GroupBits;
	const uint32* GroupBitsPtr = nullptr;
	if (GroupA.Num() > 0)
	{
		GroupBits.SetNumZeroed((View.count + 31) / 32);
		for (const int32 Index : GroupA)
		{
			if (Index >= 0 && Index < View.count)
			{
				GroupBits[Index >> 5] |= (1u << (Index & 31));
			}
		}
		GroupBitsPtr = GroupBits.GetData();
	}

	TArray<float> Projections;
	Projections.SetNumUninitialized(View.count);
	float Separation = 0.0f;
	const Status Result = superfaiss::ProjectionReport(View, PaddedDir.GetData(),
		GroupBitsPtr, Projections.GetData(), GroupBitsPtr != nullptr ? &Separation : nullptr);
	if (Result != Status::Ok)
	{
		return false;
	}
	OutProjections = MoveTemp(Projections);
	OutSeparation = Separation;
	return true;
}

// --- V2.5 analytics over a live scratch snapshot (plan section 22, T-V2.5-U2) ---

bool USuperFAISSSubsystem::BankSpreadCrossDeviceScratch(USuperFAISSScratchBank* Bank,
	ESuperFAISSReduce Reduce, float& OutValue)
{
	using namespace superfaiss;
	OutValue = 0.0f;
	if (Bank == nullptr || !Bank->IsInitialized() || !Bank->TryPin())
	{
		return false;
	}
	ON_SCOPE_EXIT { Bank->Unpin(); };

	TSharedPtr<FPooledWorkspace> Scratch = AcquireWorkspace();
	bool bOk = false;
	do
	{
		BankView View;
		// Size the tombstone staging from CAPACITY, not the current count (the P-1
		// posture QueryScratch documents): a concurrent Append can advance Snapshot's
		// own count read past a count read here; capacity is pin-stable and bounds it.
		const int32 Words = ScratchBank::TombstoneWords(Bank->Core().Capacity());
		if (Scratch->TombstoneStaging.Num() < Words)
		{
			Scratch->TombstoneStaging.SetNumZeroed(Words);
		}
		if (Bank->Core().Snapshot(&View, Scratch->TombstoneStaging.GetData()) != Status::Ok)
		{
			break;
		}
		if (View.count <= 0)
		{
			break; // an empty selection is a defined rejection (matches the baked path)
		}
		if (Scratch->XdImageStaging.Num() < View.paddedDims)
		{
			Scratch->XdImageStaging.SetNumZeroed(View.paddedDims);
		}
		// Spread over all published rows; the snapshot tombstones are the exclusion set
		// (deletion is exclusion), so a removed row joins neither the pool nor the mean.
		TArray<int32> RowIndices;
		RowIndices.SetNumUninitialized(View.count);
		for (int32 i = 0; i < View.count; ++i)
		{
			RowIndices[i] = i;
		}
		const Status Result = SpreadCrossDevice(View, RowIndices.GetData(), View.count,
			Scratch->TombstoneStaging.GetData(), static_cast<superfaiss::Reduce>(Reduce),
			Scratch->XdImageStaging.GetData(), &OutValue);
		bOk = Result == Status::Ok;
	} while (false);
	ReleaseWorkspace(MoveTemp(Scratch));
	return bOk;
}

bool USuperFAISSSubsystem::MeanNearestNeighborCrossDeviceScratch(
	USuperFAISSScratchBank* SourceBank, const USuperFAISSVectorBank* TargetBank,
	float& OutValue)
{
	using namespace superfaiss;
	OutValue = 0.0f;
	if (SourceBank == nullptr || !SourceBank->IsInitialized() ||
		TargetBank == nullptr || !TargetBank->IsValid() || !SourceBank->TryPin())
	{
		return false;
	}
	ON_SCOPE_EXIT { SourceBank->Unpin(); };

	const BankView Target = TargetBank->GetBankView();
	TSharedPtr<FPooledWorkspace> Scratch = AcquireWorkspace();
	bool bOk = false;
	do
	{
		BankView Source;
		const int32 Words = ScratchBank::TombstoneWords(SourceBank->Core().Capacity());
		if (Scratch->TombstoneStaging.Num() < Words)
		{
			Scratch->TombstoneStaging.SetNumZeroed(Words);
		}
		if (SourceBank->Core().Snapshot(&Source, Scratch->TombstoneStaging.GetData()) !=
			Status::Ok)
		{
			break;
		}
		if (Source.count <= 0)
		{
			break;
		}
		if (Scratch->XdQueryStaging.Num() < Source.count)
		{
			Scratch->XdQueryStaging.SetNumUninitialized(Source.count);
		}
		if (Scratch->HitStaging.Num() < Source.count)
		{
			Scratch->HitStaging.SetNumUninitialized(Source.count);
		}
		if (Scratch->CountStaging.Num() < Source.count)
		{
			Scratch->CountStaging.SetNumUninitialized(Source.count);
		}
		const Status Result = MeanNNCrossDevice(Source,
			Scratch->TombstoneStaging.GetData(), Target, nullptr,
			Scratch->XdQueryStaging.GetData(), Scratch->HitStaging.GetData(),
			Scratch->CountStaging.GetData(), Scratch->Core, &OutValue);
		bOk = Result == Status::Ok;
	} while (false);
	ReleaseWorkspace(MoveTemp(Scratch));
	return bOk;
}

bool USuperFAISSSubsystem::SetToSetDistanceCrossDeviceScratch(USuperFAISSScratchBank* BankA,
	const USuperFAISSVectorBank* BankB, const TArray<int32>& RowIndicesB,
	const TArray<int32>& WeightsB, ESuperFAISSBankMetric Metric, float& OutDistance)
{
	using namespace superfaiss;
	OutDistance = 0.0f;
	if (BankA == nullptr || !BankA->IsInitialized() || BankB == nullptr ||
		!BankB->IsValid() || RowIndicesB.Num() == 0)
	{
		return false;
	}
	if (WeightsB.Num() != 0 && WeightsB.Num() != RowIndicesB.Num())
	{
		return false;
	}
	if (!BankA->TryPin())
	{
		return false;
	}
	ON_SCOPE_EXIT { BankA->Unpin(); };

	const BankView ViewB = BankB->GetBankView();
	TSharedPtr<FPooledWorkspace> Scratch = AcquireWorkspace();
	bool bOk = false;
	do
	{
		BankView ViewA;
		const int32 Words = ScratchBank::TombstoneWords(BankA->Core().Capacity());
		if (Scratch->TombstoneStaging.Num() < Words)
		{
			Scratch->TombstoneStaging.SetNumZeroed(Words);
		}
		if (BankA->Core().Snapshot(&ViewA, Scratch->TombstoneStaging.GetData()) != Status::Ok)
		{
			break;
		}
		if (ViewA.count <= 0)
		{
			break;
		}
		if (Scratch->XdImageStaging.Num() < ViewA.paddedDims)
		{
			Scratch->XdImageStaging.SetNumZeroed(ViewA.paddedDims);
		}
		if (Scratch->XdImageStagingB.Num() < ViewB.paddedDims)
		{
			Scratch->XdImageStagingB.SetNumZeroed(ViewB.paddedDims);
		}
		TArray<int32> RowIndicesA;
		RowIndicesA.SetNumUninitialized(ViewA.count);
		for (int32 i = 0; i < ViewA.count; ++i)
		{
			RowIndicesA[i] = i;
		}
		const Status Result = CentroidDistanceCrossDevice(
			ViewA, RowIndicesA.GetData(), ViewA.count, nullptr,
			Scratch->TombstoneStaging.GetData(),
			ViewB, RowIndicesB.GetData(), RowIndicesB.Num(),
			WeightsB.Num() > 0 ? WeightsB.GetData() : nullptr, nullptr,
			static_cast<superfaiss::Metric>(Metric), Scratch->XdImageStaging.GetData(),
			Scratch->XdImageStagingB.GetData(), &OutDistance);
		bOk = Result == Status::Ok;
	} while (false);
	ReleaseWorkspace(MoveTemp(Scratch));
	return bOk;
}
