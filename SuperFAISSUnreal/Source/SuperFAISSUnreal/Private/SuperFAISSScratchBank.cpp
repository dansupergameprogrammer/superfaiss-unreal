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
	ESuperFAISSBankMetric Metric, ESuperFAISSBankQuantization Quantization)
{
	return Bank.Create(Capacity, Dims, ToCoreMetric(Metric), ToCoreQuant(Quantization)) ==
		superfaiss::Status::Ok;
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
	OutIndexMap.Reset();
	if (!Bank.IsCreated())
	{
		return nullptr;
	}

	USuperFAISSVectorBank* Frozen = nullptr;
	const bool bOk = DrainAndRun([this, &OutIndexMap, &Frozen]() {
		using namespace superfaiss;
		const int32 Count = Bank.Count();
		const int32 Live = Bank.FreezeLiveCount();
		const int32 Pd = Bank.GetPaddedDims();
		const bool bInt8 = Bank.GetQuantization() == Quantization::Int8;

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
				OutIndexMap.GetData()) != Status::Ok)
		{
			return false;
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
	return DrainAndRun([this, &Bytes]() {
		FByteReader Reader;
		Reader.Bytes = &Bytes;
		superfaiss::ScratchArchive Archive;
		Archive.read = &FByteReader::Read;
		Archive.user = &Reader;
		return Bank.Load(Archive) == superfaiss::Status::Ok;
	});
}
