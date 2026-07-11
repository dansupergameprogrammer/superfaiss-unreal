#include "SuperFAISSScratchBank.h"

namespace
{
	superfaiss::Metric ToCoreMetric(ESuperFAISSBankMetric M)
	{
		return static_cast<superfaiss::Metric>(M);
	}

	superfaiss::Quantization ToCoreQuant(ESuperFAISSBankQuantization Q)
	{
		return static_cast<superfaiss::Quantization>(Q);
	}

	// Archive adapters over TArray<uint8>.
	struct FByteWriter
	{
		TArray<uint8>* Bytes = nullptr;
		static bool Write(void* User, const void* Data, size_t N)
		{
			auto* Self = static_cast<FByteWriter*>(User);
			Self->Bytes->Append(static_cast<const uint8*>(Data), static_cast<int64>(N));
			return true;
		}
	};

	struct FByteReader
	{
		const TArray<uint8>* Bytes = nullptr;
		int64 Pos = 0;
		static bool Read(void* User, void* Data, size_t N)
		{
			auto* Self = static_cast<FByteReader*>(User);
			if (Self->Pos + static_cast<int64>(N) > Self->Bytes->Num())
			{
				return false;
			}
			FMemory::Memcpy(Data, Self->Bytes->GetData() + Self->Pos, N);
			Self->Pos += static_cast<int64>(N);
			return true;
		}
	};
} // namespace

bool USuperFAISSScratchBank::Init(int32 Capacity, int32 Dims,
	ESuperFAISSBankMetric Metric, ESuperFAISSBankQuantization Quantization,
	bool bRetainFloats)
{
	return Bank.Create(Capacity, Dims, ToCoreMetric(Metric), ToCoreQuant(Quantization),
			   bRetainFloats) == superfaiss::Status::Ok;
}

void USuperFAISSScratchBank::FillReport(
	const superfaiss::ScratchRecallReport& Core, FSuperFAISSScratchRecallReport& Out)
{
	Out.Recall = Core.recall;
	Out.K = Core.k;
	Out.SampleCount = Core.sampleCount;
	Out.LiveRows = Core.liveRows;
	Out.Seed = static_cast<int64>(Core.seed);
	Out.Generation = static_cast<int64>(Core.generation);
	Out.bInformative = Core.informative;
}

bool USuperFAISSScratchBank::MeasureRecall(FSuperFAISSScratchRecallReport& OutReport)
{
	OutReport = FSuperFAISSScratchRecallReport{};
	if (!Bank.IsCreated() || !Bank.RetainsFloats())
	{
		// The core's InvalidArgument, mapped: a non-retention bank has no reference
		// to audit against — a defined rejection, never a guessed number.
		return false;
	}
	// The N4 posture: refuse while a drain-requiring operation is waiting, like any
	// query dispatch. The pin is released before the core call — the core takes its
	// own reader pin for the sweep's flight; holding ours across it would deadlock a
	// drain that starts in between.
	if (!Bank.TryPinReader())
	{
		return false;
	}
	Bank.UnpinReader();

	superfaiss::ScratchRecallReport Core;
	if (Bank.MeasureScratchRecall(RecallWorkspace, &Core) != superfaiss::Status::Ok)
	{
		return false;
	}
	FillReport(Core, OutReport);
	LastRecallReport = OutReport;
	bHasRecallReport = true;
	return true;
}

bool USuperFAISSScratchBank::GetLastRecallReport(
	FSuperFAISSScratchRecallReport& OutReport, bool& bOutStale) const
{
	OutReport = FSuperFAISSScratchRecallReport{};
	bOutStale = false;
	if (!bHasRecallReport)
	{
		return false;
	}
	OutReport = LastRecallReport;
	bOutStale = IsRecallReportStale(LastRecallReport);
	return true;
}

bool USuperFAISSScratchBank::Append(const TArray<float>& Vector, int32& OutIndex)
{
	OutIndex = INDEX_NONE;
	return Bank.Append(Vector.GetData(), Vector.Num(), &OutIndex) == superfaiss::Status::Ok;
}

bool USuperFAISSScratchBank::Remove(int32 Index)
{
	return Bank.Remove(Index) == superfaiss::Status::Ok;
}

bool USuperFAISSScratchBank::DrainAndRun(TFunctionRef<bool()> Op)
{
	// Core BeginExclusive refuses new pins (the subsystem's TryPin fails at the
	// dispatch gate, T-044 N4) and waits the in-flight ones out with the seq_cst
	// pairing the protocol's safety requires (Poirot F4). A false return means
	// another exclusive operation is in progress: writer-coordination misuse.
	if (!Bank.BeginExclusive())
	{
		return false;
	}
	const bool bOk = Op();
	Bank.EndExclusive();
	return bOk;
}

bool USuperFAISSScratchBank::Grow(int32 NewCapacity)
{
	if (!Bank.IsCreated())
	{
		return false;
	}
	return DrainAndRun([this, NewCapacity]() {
		return Bank.Grow(NewCapacity) == superfaiss::Status::Ok;
	});
}

USuperFAISSVectorBank* USuperFAISSScratchBank::Freeze(TArray<int32>& OutIndexMap)
{
	return FreezeInternal(OutIndexMap, nullptr, nullptr);
}

USuperFAISSVectorBank* USuperFAISSScratchBank::FreezeWithRecall(
	TArray<int32>& OutIndexMap, FSuperFAISSScratchRecallReport& OutRecallReport,
	bool& bOutRecallMeasured)
{
	OutRecallReport = FSuperFAISSScratchRecallReport{};
	bOutRecallMeasured = false;
	superfaiss::ScratchRecallReport Core;
	bool bMeasured = false;
	USuperFAISSVectorBank* Frozen = FreezeInternal(OutIndexMap, &Core, &bMeasured);
	if (Frozen != nullptr && bMeasured)
	{
		FillReport(Core, OutRecallReport);
		bOutRecallMeasured = true;
	}
	return Frozen;
}

USuperFAISSVectorBank* USuperFAISSScratchBank::FreezeInternal(TArray<int32>& OutIndexMap,
	superfaiss::ScratchRecallReport* OutCoreReport, bool* bOutMeasured)
{
	OutIndexMap.Reset();
	if (!Bank.IsCreated())
	{
		return nullptr;
	}

	USuperFAISSVectorBank* Frozen = nullptr;
	const bool bOk = DrainAndRun([this, &OutIndexMap, &Frozen, OutCoreReport,
							 bOutMeasured]() {
		using namespace superfaiss;
		const int32 Count = Bank.Count();
		const int32 Live = Bank.FreezeLiveCount();
		const int32 Pd = Bank.GetPaddedDims();
		const bool bInt8 = Bank.GetQuantization() == Quantization::Int8;

		// The V2.3 re-measurement: on a retention bank the core Freeze measures the
		// compacted rows at freeze time (inside this exclusive window); a
		// non-retention freeze produces no number — the caller's flag stays false.
		ScratchRecallReport* CoreReport =
			(OutCoreReport != nullptr && Bank.RetainsFloats()) ? OutCoreReport : nullptr;
		Workspace* CoreWs = CoreReport != nullptr ? &RecallWorkspace : nullptr;

		// Zero live rows is a legitimate freeze (an empty memory graduating is
		// still a memory): produce an EMPTY valid bank, not a failure a caller
		// cannot tell from a real one (Poirot R-5). Core Freeze rightly rejects a
		// null output buffer, so the empty case short-circuits before it.
		if (Live == 0)
		{
			OutIndexMap.SetNumUninitialized(Count);
			for (int32 R = 0; R < Count; ++R)
			{
				OutIndexMap[R] = -1;
			}
			USuperFAISSVectorBank* Empty = NewObject<USuperFAISSVectorBank>(GetOuter());
			FString EmptyError;
			if (!Empty->InitFromBaked(nullptr, nullptr, 0, Bank.Dims(), GetMetric(),
					GetQuantization(), TEXT("frozen-scratch-empty"), EmptyError))
			{
				UE_LOG(LogTemp, Error, TEXT("SuperFAISSScratchBank empty freeze: %s"),
					*EmptyError);
				return false;
			}
			Frozen = Empty;
			return true;
		}

		TArray<uint8, TAlignedHeapAllocator<16>> Rows;
		Rows.SetNumZeroed(static_cast<int64>(Live) * Pd * ElementSize(Bank.GetQuantization()));
		TArray<float> Scales;
		if (bInt8)
		{
			Scales.SetNumZeroed(Live);
		}
		OutIndexMap.SetNumUninitialized(Count);
		if (Bank.Freeze(Rows.GetData(), bInt8 ? Scales.GetData() : nullptr,
				OutIndexMap.GetData(), CoreReport, CoreWs) != Status::Ok)
		{
			return false;
		}
		if (CoreReport != nullptr && bOutMeasured != nullptr)
		{
			*bOutMeasured = true;
		}

		USuperFAISSVectorBank* Candidate = NewObject<USuperFAISSVectorBank>(GetOuter());
		FString Error;
		// The payload is copied, not re-baked: rows were normalized/quantized at
		// append with the importer's math, so the frozen bank is bit-identical.
		if (!Candidate->InitFromBaked(Rows.GetData(), bInt8 ? Scales.GetData() : nullptr,
				Live, Bank.Dims(), GetMetric(), GetQuantization(),
				TEXT("frozen-scratch"), Error))
		{
			UE_LOG(LogTemp, Error, TEXT("SuperFAISSScratchBank freeze: %s"), *Error);
			return false;
		}
		Frozen = Candidate;
		return true;
	});
	if (!bOk)
	{
		OutIndexMap.Reset();
		return nullptr;
	}
	return Frozen;
}

bool USuperFAISSScratchBank::SaveToBytes(TArray<uint8>& OutBytes) const
{
	OutBytes.Reset();
	FByteWriter Writer;
	Writer.Bytes = &OutBytes;
	superfaiss::ScratchArchive Archive;
	Archive.write = &FByteWriter::Write;
	Archive.user = &Writer;
	return Bank.Save(Archive) == superfaiss::Status::Ok;
}

bool USuperFAISSScratchBank::LoadFromBytes(const TArray<uint8>& Bytes)
{
	// Load is exclusive: drain queries; the core's reject-over-degrade keeps the
	// current state on a bad blob.
	const bool bLoaded = DrainAndRun([this, &Bytes]() {
		FByteReader Reader;
		Reader.Bytes = &Bytes;
		superfaiss::ScratchArchive Archive;
		Archive.read = &FByteReader::Read;
		Archive.user = &Reader;
		return Bank.Load(Archive) == superfaiss::Status::Ok;
	});
	if (bLoaded)
	{
		// A Load replaces the rows wholesale (review S1): the cached report
		// describes rows that no longer exist, so it is dropped — a stale number
		// must never wear a current face across a restore. (The core's generation
		// also advances past every prior stamp, so a caller-held report reads
		// stale too; a failed load keeps the bank unchanged and the cache with it.)
		bHasRecallReport = false;
		LastRecallReport = FSuperFAISSScratchRecallReport{};
	}
	return bLoaded;
}
