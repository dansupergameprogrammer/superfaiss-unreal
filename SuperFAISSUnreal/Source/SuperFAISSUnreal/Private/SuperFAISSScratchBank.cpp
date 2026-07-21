#include "SuperFAISSScratchBank.h"

#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "SuperFAISSSubsystem.h" // the shared SuperFAISS trace channel (plan section 5.1)

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

	// The host-side channel-name frame (S2). The core scratch archive carries the
	// channel table's element RANGES but not the host FName vocabulary — the core never
	// sees names — so a save/load round trip would restore the core channels with an empty
	// host name list, and every named-channel query on the loaded bank would miss. The
	// plugin appends this frame AFTER the core archive: the core Load reads exactly the
	// archive and stops, so a legacy (frame-less) blob loads unchanged and a new blob loads
	// unchanged in an older plugin (the trailing frame is ignored) — back-compatible both
	// ways. Format: int32 count, then per channel { int32 utf8-len, utf8 bytes, int32
	// offset, int32 length }.
	void WriteI32(TArray<uint8>& Out, int32 V)
	{
		Out.Append(reinterpret_cast<const uint8*>(&V), sizeof(int32));
	}

	void WriteChannelFrame(TArray<uint8>& Out, const TArray<FName>& Names,
		const TArray<int32>& Offsets, const TArray<int32>& Lengths)
	{
		WriteI32(Out, Names.Num());
		for (int32 C = 0; C < Names.Num(); ++C)
		{
			const FString S = Names[C].ToString();
			const FTCHARToUTF8 Utf8(*S);
			WriteI32(Out, Utf8.Length());
			Out.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
			WriteI32(Out, Offsets[C]);
			WriteI32(Out, Lengths[C]);
		}
	}

	bool ReadI32(FByteReader& R, int32& OutV)
	{
		return FByteReader::Read(&R, &OutV, sizeof(int32));
	}

	// Parses the frame into temporaries; returns false (adopting nothing) on any short or
	// malformed read, so a corrupt trailer degrades to a name-less load rather than a crash.
	bool ReadChannelFrame(FByteReader& R, TArray<FName>& OutNames,
		TArray<int32>& OutOffsets, TArray<int32>& OutLengths)
	{
		int32 Count = 0;
		if (!ReadI32(R, Count) || Count < 0 || Count > superfaiss::kMaxChannels)
		{
			return false;
		}
		OutNames.Reset();
		OutOffsets.Reset();
		OutLengths.Reset();
		for (int32 C = 0; C < Count; ++C)
		{
			int32 Len = 0;
			if (!ReadI32(R, Len) || Len < 0 || Len > 65536)
			{
				return false;
			}
			TArray<uint8> Buf;
			Buf.SetNumUninitialized(Len + 1);
			if (Len > 0 && !FByteReader::Read(&R, Buf.GetData(), Len))
			{
				return false;
			}
			Buf[Len] = 0;
			int32 Offset = 0;
			int32 Length = 0;
			if (!ReadI32(R, Offset) || !ReadI32(R, Length))
			{
				return false;
			}
			OutNames.Add(FName(UTF8_TO_TCHAR(reinterpret_cast<const char*>(Buf.GetData()))));
			OutOffsets.Add(Offset);
			OutLengths.Add(Length);
		}
		return true;
	}
} // namespace

bool USuperFAISSScratchBank::Init(int32 Capacity, int32 Dims,
	ESuperFAISSBankMetric Metric, ESuperFAISSBankQuantization Quantization,
	bool bRetainFloats)
{
	return Bank.Create(Capacity, Dims, ToCoreMetric(Metric), ToCoreQuant(Quantization),
			   bRetainFloats) == superfaiss::Status::Ok;
}

bool USuperFAISSScratchBank::InitWithChannels(int32 Capacity, int32 Dims,
	ESuperFAISSBankMetric Metric, ESuperFAISSBankQuantization Quantization,
	const TArray<FName>& InChannelNames,
	const TArray<int32>& InChannelOffsets,
	const TArray<int32>& InChannelLengths,
	bool bRetainFloats)
{
	// Parallel-array agreement is a host-side contract — the core never sees the
	// names, so a mismatched Names/Offsets/Lengths table is a defined rejection here,
	// before anything is allocated (mirrors the baked RebuildChannelTable refusal).
	if (InChannelNames.Num() != InChannelOffsets.Num() ||
		InChannelNames.Num() != InChannelLengths.Num())
	{
		return false;
	}
	// Duplicate names would make GetChannelIndex ambiguous — also host-side, also
	// rejected before allocation.
	for (int32 A = 0; A < InChannelNames.Num(); ++A)
	{
		for (int32 B = A + 1; B < InChannelNames.Num(); ++B)
		{
			if (InChannelNames[A] == InChannelNames[B])
			{
				return false;
			}
		}
	}

	// Build the core-facing channel table (padded-grid ranges), mirroring the baked
	// bank's RebuildChannelTable: a channel ending at Dims extends across the zero
	// pad lanes so its stored range stays on the element grid (pads contribute
	// nothing). Range validity — in-bounds, ascending, non-overlapping, on the
	// 16-byte element grid, count in [1, kMaxChannels] — is the core Create's
	// contract; a malformed range fails there and leaves the bank uncreated.
	const int32 PaddedDims = superfaiss::PaddedDims(Dims, ToCoreQuant(Quantization));
	TArray<superfaiss::ChannelInfo> Table;
	Table.Reserve(InChannelNames.Num());
	for (int32 C = 0; C < InChannelNames.Num(); ++C)
	{
		superfaiss::ChannelInfo Info;
		Info.offset = InChannelOffsets[C];
		Info.length = (InChannelOffsets[C] + InChannelLengths[C] == Dims)
			? PaddedDims - InChannelOffsets[C]
			: InChannelLengths[C];
		Table.Add(Info);
	}

	const superfaiss::ChannelInfo* Channels = Table.Num() > 0 ? Table.GetData() : nullptr;
	if (Bank.Create(Capacity, Dims, ToCoreMetric(Metric), ToCoreQuant(Quantization),
			Channels, Table.Num(), bRetainFloats) != superfaiss::Status::Ok)
	{
		return false;
	}

	// Store the host-side vocabulary only after a successful create — a rejected
	// table leaves the bank uninitialized and channel-less (reject-over-degrade,
	// the posture Init and LoadFromBytes hold elsewhere on this class).
	ChannelNames = InChannelNames;
	ChannelOffsets = InChannelOffsets;
	ChannelLengths = InChannelLengths;
	return true;
}

bool USuperFAISSScratchBank::Relabel(const TArray<FName>& InChannelNames,
	const TArray<int32>& InChannelOffsets, const TArray<int32>& InChannelLengths)
{
	if (!Bank.IsCreated())
	{
		return false;
	}
	// Host-side table validation, identical to InitWithChannels: the core never sees the
	// names, so a mismatched Names/Offsets/Lengths table or a duplicate name is a defined
	// rejection here — evaluated BEFORE the drain, so a malformed request never disturbs
	// live readers. An empty table is legal: it demotes the bank to single-space.
	if (InChannelNames.Num() != InChannelOffsets.Num() ||
		InChannelNames.Num() != InChannelLengths.Num())
	{
		return false;
	}
	for (int32 A = 0; A < InChannelNames.Num(); ++A)
	{
		for (int32 B = A + 1; B < InChannelNames.Num(); ++B)
		{
			if (InChannelNames[A] == InChannelNames[B])
			{
				return false;
			}
		}
	}

	// Build the core-facing channel table on the padded grid, mirroring InitWithChannels
	// (a channel ending at Dims extends across the zero pad lanes). Range validity —
	// in-bounds, ascending, non-overlapping, on the 16-byte element grid — is the core
	// Relabel's contract; a malformed range fails there, reject-over-degrade, leaving the
	// bank under the old table.
	const int32 Dims = Bank.Dims();
	const int32 PaddedDims = superfaiss::PaddedDims(Dims, Bank.GetQuantization());
	TArray<superfaiss::ChannelInfo> Table;
	Table.Reserve(InChannelNames.Num());
	for (int32 C = 0; C < InChannelNames.Num(); ++C)
	{
		superfaiss::ChannelInfo Info;
		Info.offset = InChannelOffsets[C];
		Info.length = (InChannelOffsets[C] + InChannelLengths[C] == Dims)
			? PaddedDims - InChannelOffsets[C]
			: InChannelLengths[C];
		Table.Add(Info);
	}

	// Relabel is EXCLUSIVE (drains readers, refuses new pins) like Grow/Load; the core
	// applies the swap reject-over-degrade and advances the generation on success.
	const superfaiss::ChannelInfo* Channels = Table.Num() > 0 ? Table.GetData() : nullptr;
	return DrainAndRun([this, Channels, &Table, &InChannelNames, &InChannelOffsets,
						   &InChannelLengths]() {
		if (Bank.Relabel(Channels, Table.Num()) != superfaiss::Status::Ok)
		{
			return false;
		}
		// Adopt the host-side vocabulary INSIDE the exclusive window, published by the
		// same EndExclusive/TryPin seq_cst pairing that publishes the core table swap
		// (C1). The host name list (`GetChannelIndex` reads it on the reader
		// side) and the core channel table are two objects that must move as one; doing
		// it here means no pinned reader ever observes host names skewed against the core
		// table. A rejected relabel returns above, leaving the old names in place
		// (reject-over-degrade); a demote (empty table) clears them.
		ChannelNames = InChannelNames;
		ChannelOffsets = InChannelOffsets;
		ChannelLengths = InChannelLengths;
		return true;
	});
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

bool USuperFAISSScratchBank::MeasureRecallPerChannel(
	TArray<FSuperFAISSScratchRecallReport>& OutReports)
{
	OutReports.Reset();
	const int32 ChannelCount = ChannelNames.Num();
	if (!Bank.IsCreated() || !Bank.RetainsFloats() || ChannelCount <= 0)
	{
		// The core's InvalidArgument, mapped: a non-retention bank has no reference to
		// audit, and a channel-less bank has nothing to scope per channel (MeasureRecall
		// is its surface). A defined rejection, never a guessed number.
		return false;
	}
	// The N4 posture: refuse while a drain-requiring operation is waiting, like any
	// query dispatch. The pin is released before the core call — the core takes its own
	// reader pin for the sweep's flight; holding ours across it would deadlock a drain
	// that starts in between (the MeasureRecall idiom).
	if (!Bank.TryPinReader())
	{
		return false;
	}
	Bank.UnpinReader();

	TArray<superfaiss::ScratchRecallReport> CoreReports;
	CoreReports.SetNum(ChannelCount);
	if (Bank.MeasureScratchRecallPerChannel(RecallWorkspace, CoreReports.GetData(),
			ChannelCount) != superfaiss::Status::Ok)
	{
		return false;
	}
	OutReports.SetNum(ChannelCount);
	for (int32 C = 0; C < ChannelCount; ++C)
	{
		FillReport(CoreReports[C], OutReports[C]);
	}
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
	// pairing the protocol's safety requires (F4). A false return means
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
		// cannot tell from a real one (R-5). Core Freeze rightly rejects a
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
	// "bank load/serialization" (plugin plan section 5.1) — the save direction.
	TRACE_CPUPROFILER_EVENT_SCOPE_ON_CHANNEL(TEXT("SuperFAISS.BankSerialize"), SuperFAISS);
	OutBytes.Reset();
	FByteWriter Writer;
	Writer.Bytes = &OutBytes;
	superfaiss::ScratchArchive Archive;
	Archive.write = &FByteWriter::Write;
	Archive.user = &Writer;
	if (Bank.Save(Archive) != superfaiss::Status::Ok)
	{
		return false;
	}
	// Append the host-side channel-name frame the core archive cannot carry (S2).
	WriteChannelFrame(OutBytes, ChannelNames, ChannelOffsets, ChannelLengths);
	return true;
}

bool USuperFAISSScratchBank::LoadFromBytes(const TArray<uint8>& Bytes)
{
	// "bank load/serialization" (plugin plan section 5.1) — the load direction, plus
	// the bank load/unload Insights bookmark ("one of the four named events a
	// developer hunts for in a timeline").
	TRACE_CPUPROFILER_EVENT_SCOPE_ON_CHANNEL(TEXT("SuperFAISS.BankSerialize"), SuperFAISS);
	TRACE_BOOKMARK(TEXT("SuperFAISS: scratch bank load"));
	// Load is exclusive: drain queries; the core's reject-over-degrade keeps the
	// current state on a bad blob.
	const bool bLoaded = DrainAndRun([this, &Bytes]() {
		FByteReader Reader;
		Reader.Bytes = &Bytes;
		superfaiss::ScratchArchive Archive;
		Archive.read = &FByteReader::Read;
		Archive.user = &Reader;
		if (Bank.Load(Archive) != superfaiss::Status::Ok)
		{
			return false;
		}
		// The core Load succeeded and replaced the rows; the pre-load host vocabulary is
		// now stale, so it is cleared and repopulated from the appended frame (S2).
		// The core reader stopped exactly at the archive end, so anything left is the
		// frame; a legacy (frame-less) blob leaves the loaded bank host-channel-less, and
		// a malformed or count-mismatched frame is ignored rather than half-adopted. Done
		// inside the exclusive window so the host names publish with the loaded core table
		// (the C1 discipline), never observed skewed by a pinned reader.
		ChannelNames.Reset();
		ChannelOffsets.Reset();
		ChannelLengths.Reset();
		if (Reader.Pos < Bytes.Num())
		{
			TArray<FName> Names;
			TArray<int32> Offsets;
			TArray<int32> Lengths;
			if (ReadChannelFrame(Reader, Names, Offsets, Lengths) &&
				Names.Num() == Bank.GetChannelCount())
			{
				ChannelNames = MoveTemp(Names);
				ChannelOffsets = MoveTemp(Offsets);
				ChannelLengths = MoveTemp(Lengths);
			}
		}
		return true;
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
