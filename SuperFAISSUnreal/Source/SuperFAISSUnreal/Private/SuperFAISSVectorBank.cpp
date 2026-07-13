#include "SuperFAISSVectorBank.h"

#include "superfaiss/superfaiss.h"

namespace
{
	superfaiss::Quantization ToCore(ESuperFAISSBankQuantization Q)
	{
		return Q == ESuperFAISSBankQuantization::Float32
			? superfaiss::Quantization::Float32
			: superfaiss::Quantization::Int8;
	}

	superfaiss::Metric ToCore(ESuperFAISSBankMetric M)
	{
		switch (M)
		{
		case ESuperFAISSBankMetric::Dot: return superfaiss::Metric::Dot;
		case ESuperFAISSBankMetric::L2: return superfaiss::Metric::L2;
		default: return superfaiss::Metric::Cosine;
		}
	}
}

bool USuperFAISSVectorBank::InitFromSource(
	TConstArrayView<float> SourceRows,
	int32 InCount,
	int32 InDims,
	ESuperFAISSBankMetric InMetric,
	ESuperFAISSBankQuantization InQuantization,
	TConstArrayView<FName> InIds,
	const FString& InSourceHash,
	FString& OutError,
	TConstArrayView<FName> InChannelNames,
	TConstArrayView<int32> InChannelOffsets,
	TConstArrayView<int32> InChannelLengths)
{
	using namespace superfaiss;

	bValidated = false;
	OutError.Reset();

	if (InCount < 0 || InDims <= 0 ||
		SourceRows.Num() != static_cast<int64>(InCount) * InDims)
	{
		OutError = TEXT("source dimensions disagree with the row payload");
		return false;
	}
	if (InIds.Num() != 0 && InIds.Num() != InCount)
	{
		OutError = FString::Printf(TEXT("%d ids for %d rows"), InIds.Num(), InCount);
		return false;
	}
	if (InIds.Num() != 0)
	{
		TSet<FName> Unique;
		Unique.Append(InIds);
		if (Unique.Num() != InIds.Num())
		{
			OutError = TEXT("ids are not unique");
			return false;
		}
	}

	// Working copy: normalization mutates.
	TArray<float> Rows(SourceRows.GetData(), SourceRows.Num());

	int32_t BadRow = -1;
	if (ValidateSourceRows(Rows.GetData(), InCount, InDims, &BadRow) != Status::Ok)
	{
		OutError = FString::Printf(TEXT("non-finite value in source row %d"), BadRow);
		return false;
	}
	if (InMetric == ESuperFAISSBankMetric::Cosine && InCount > 0)
	{
		if (NormalizeRows(Rows.GetData(), InCount, InDims, &BadRow) != Status::Ok)
		{
			OutError = FString::Printf(TEXT("zero-norm source row %d in a Cosine bank"), BadRow);
			return false;
		}
	}

	const superfaiss::Quantization CoreQuant = ToCore(InQuantization);
	const int32 Pd = PaddedDims_Internal(InDims, CoreQuant);
	const int64 Bytes = static_cast<int64>(InCount) * Pd * ElementSize(CoreQuant);
	if (Bytes > MAX_int32)
	{
		OutError = FString::Printf(TEXT("bank payload %lld bytes exceeds the 2 GB asset limit"),
			static_cast<long long>(Bytes));
		return false;
	}
	Payload.SetNumZeroed(Bytes > 0 ? Bytes : 0);
	Scales.Reset();

	if (InCount > 0)
	{
		if (CoreQuant == superfaiss::Quantization::Float32)
		{
			PadRowsFloat32(Rows.GetData(), InCount, InDims, Pd,
				reinterpret_cast<float*>(Payload.GetData()));
		}
		else
		{
			Scales.SetNumZeroed(InCount);
			QuantizeRowsInt8(Rows.GetData(), InCount, InDims, Pd,
				reinterpret_cast<int8_t*>(Payload.GetData()), Scales.GetData());
		}
	}

	// Channel table (schemaVersion 2): grid-aligned offsets; a length may end
	// exactly at dims (the stored table extends it across the zero pad lanes).
	ChannelNames.Reset();
	ChannelOffsets.Reset();
	ChannelLengths.Reset();
	ChannelRecallAt10.Reset();
	ChannelInvNorms.Reset();
	// Re-init drops every measured number (Poirot R-4): the importer re-measures
	// on its own paths; anything it does not re-measure must read "not measured",
	// never a previous object's truth.
	RecallAt10 = -1.0f;
	RecallSeed = 0;
	CrossDeviceRecallAt10 = -1.0f;
	if (InChannelNames.Num() > 0)
	{
		const int32 ChannelCount = InChannelNames.Num();
		if (ChannelCount > superfaiss::kMaxChannels ||
			InChannelOffsets.Num() != ChannelCount ||
			InChannelLengths.Num() != ChannelCount)
		{
			OutError = FString::Printf(TEXT("bad channel table (%d names, %d offsets, %d lengths; max %d)"),
				ChannelCount, InChannelOffsets.Num(), InChannelLengths.Num(),
				superfaiss::kMaxChannels);
			return false;
		}
		const int32 Grid = superfaiss::kAlignment / superfaiss::ElementSize(CoreQuant);
		TSet<FName> UniqueNames;
		int32 PrevEnd = 0;
		for (int32 C = 0; C < ChannelCount; ++C)
		{
			const int32 Offset = InChannelOffsets[C];
			const int32 Length = InChannelLengths[C];
			const bool bEndsAtDims = Offset + Length == InDims;
			if (InChannelNames[C].IsNone() || Offset < 0 || Length <= 0 ||
				Offset % Grid != 0 || (Length % Grid != 0 && !bEndsAtDims) ||
				Offset < PrevEnd || Offset + Length > InDims)
			{
				OutError = FString::Printf(
					TEXT("channel %d ('%s') violates the channel rules (grid %d, ascending, within dims)"),
					C, *InChannelNames[C].ToString(), Grid);
				return false;
			}
			UniqueNames.Add(InChannelNames[C]);
			PrevEnd = Offset + Length;
		}
		if (UniqueNames.Num() != ChannelCount)
		{
			OutError = TEXT("channel names are not unique");
			return false;
		}
		ChannelNames = TArray<FName>(InChannelNames.GetData(), InChannelNames.Num());
		ChannelOffsets = TArray<int32>(InChannelOffsets.GetData(), InChannelOffsets.Num());
		ChannelLengths = TArray<int32>(InChannelLengths.GetData(), InChannelLengths.Num());
	}

	SchemaVersion = ChannelNames.Num() > 0 ? 2 : 1;
	Dims = InDims;
	PaddedDims = Pd;
	Count = InCount;
	Quantization = InQuantization;
	Metric = InMetric;
	Ids = TArray<FName>(InIds.GetData(), InIds.Num());
	SourceHash = InSourceHash;
	IndexBlockVersion = 0;
	IndexBlockData.Reset();

	RebuildChannelTable();

	// Cosine channel banks bake per-row inverse sub-norms from the QUANTIZED payload
	// (T-044 W2a): the reported per-channel cosine is the cosine of what the kernel
	// dots. A zero-norm row channel stores 0 (W3 row side).
	if (ChannelNames.Num() > 0 && InMetric == ESuperFAISSBankMetric::Cosine && InCount > 0)
	{
		ChannelInvNorms.SetNumZeroed(static_cast<int64>(InCount) * ChannelNames.Num());
		superfaiss::BankView NormView;
		NormView.rows = Payload.GetData();
		NormView.scales = Quantization == ESuperFAISSBankQuantization::Int8
			? Scales.GetData() : nullptr;
		NormView.count = InCount;
		NormView.dims = InDims;
		NormView.paddedDims = Pd;
		NormView.quant = CoreQuant;
		NormView.metric = superfaiss::Metric::Cosine;
		NormView.channels = ChannelTable.GetData();
		NormView.channelCount = ChannelTable.Num();
		if (ComputeChannelInverseNorms(NormView, ChannelInvNorms.GetData()) !=
			superfaiss::Status::Ok)
		{
			OutError = TEXT("channel inverse-norm bake failed");
			return false;
		}
	}

	return ValidateContent(OutError);
}

bool USuperFAISSVectorBank::InitFromBaked(
	const void* BakedRows,
	const float* BakedScales,
	int32 InCount,
	int32 InDims,
	ESuperFAISSBankMetric InMetric,
	ESuperFAISSBankQuantization InQuantization,
	const FString& InSourceHash,
	FString& OutError)
{
	using namespace superfaiss;

	bValidated = false;
	OutError.Reset();

	const bool bInt8 = InQuantization == ESuperFAISSBankQuantization::Int8;
	if (InCount < 0 || InDims <= 0 || (InCount > 0 && BakedRows == nullptr) ||
		(bInt8 && InCount > 0 && BakedScales == nullptr))
	{
		OutError = TEXT("bad baked-init arguments");
		return false;
	}

	const superfaiss::Quantization CoreQuant = ToCore(InQuantization);
	const int32 Pd = PaddedDims_Internal(InDims, CoreQuant);
	const int64 Bytes = static_cast<int64>(InCount) * Pd * ElementSize(CoreQuant);
	if (Bytes > MAX_int32)
	{
		OutError = FString::Printf(TEXT("bank payload %lld bytes exceeds the 2 GB asset limit"),
			static_cast<long long>(Bytes));
		return false;
	}
	Payload.SetNumZeroed(Bytes > 0 ? Bytes : 0);
	if (InCount > 0)
	{
		FMemory::Memcpy(Payload.GetData(), BakedRows, Bytes);
	}
	Scales.Reset();
	if (bInt8 && InCount > 0)
	{
		Scales.SetNumZeroed(InCount);
		FMemory::Memcpy(Scales.GetData(), BakedScales, static_cast<int64>(InCount) * sizeof(float));
	}

	ChannelNames.Reset();
	ChannelOffsets.Reset();
	ChannelLengths.Reset();
	ChannelRecallAt10.Reset();
	ChannelInvNorms.Reset();
	SchemaVersion = 1;
	Dims = InDims;
	PaddedDims = Pd;
	Count = InCount;
	Quantization = InQuantization;
	Metric = InMetric;
	Ids.Reset();
	SourceHash = InSourceHash;
	RecallAt10 = -1.0f;
	RecallSeed = 0;
	CrossDeviceRecallAt10 = -1.0f; // Poirot R-4
	IndexBlockVersion = 0;
	IndexBlockData.Reset();
	RebuildChannelTable();

	return ValidateContent(OutError);
}

bool USuperFAISSVectorBank::InitFromBaked(
	const void* BakedRows,
	const float* BakedScales,
	int32 InCount,
	int32 InDims,
	ESuperFAISSBankMetric InMetric,
	ESuperFAISSBankQuantization InQuantization,
	const FString& InSourceHash,
	FString& OutError,
	TConstArrayView<FName> InChannelNames,
	TConstArrayView<int32> InChannelOffsets,
	TConstArrayView<int32> InChannelLengths,
	TConstArrayView<float> InChannelInvNorms)
{
	using namespace superfaiss;

	bValidated = false;
	OutError.Reset();

	const bool bInt8 = InQuantization == ESuperFAISSBankQuantization::Int8;
	if (InCount < 0 || InDims <= 0 || (InCount > 0 && BakedRows == nullptr) ||
		(bInt8 && InCount > 0 && BakedScales == nullptr))
	{
		OutError = TEXT("bad baked-init arguments");
		return false;
	}

	const superfaiss::Quantization CoreQuant = ToCore(InQuantization);
	const int32 Pd = PaddedDims_Internal(InDims, CoreQuant);
	const int64 Bytes = static_cast<int64>(InCount) * Pd * ElementSize(CoreQuant);
	if (Bytes > MAX_int32)
	{
		OutError = FString::Printf(TEXT("bank payload %lld bytes exceeds the 2 GB asset limit"),
			static_cast<long long>(Bytes));
		return false;
	}
	Payload.SetNumZeroed(Bytes > 0 ? Bytes : 0);
	if (InCount > 0)
	{
		FMemory::Memcpy(Payload.GetData(), BakedRows, Bytes);
	}
	Scales.Reset();
	if (bInt8 && InCount > 0)
	{
		Scales.SetNumZeroed(InCount);
		FMemory::Memcpy(Scales.GetData(), BakedScales, static_cast<int64>(InCount) * sizeof(float));
	}

	// The channel table (schema-2): validate the same rules the import path applies so a
	// malformed graduation is a defined reject, not a silently wrong bank.
	ChannelNames.Reset();
	ChannelOffsets.Reset();
	ChannelLengths.Reset();
	ChannelRecallAt10.Reset();
	ChannelInvNorms.Reset();
	const int32 ChannelCount = InChannelNames.Num();
	if (ChannelCount <= 0 || ChannelCount > superfaiss::kMaxChannels ||
		InChannelOffsets.Num() != ChannelCount || InChannelLengths.Num() != ChannelCount)
	{
		OutError = TEXT("bad channel table for channel-aware baked init");
		return false;
	}
	const int32 Grid = superfaiss::kAlignment / superfaiss::ElementSize(CoreQuant);
	TSet<FName> UniqueNames;
	int32 PrevEnd = 0;
	for (int32 C = 0; C < ChannelCount; ++C)
	{
		const int32 Offset = InChannelOffsets[C];
		const int32 Length = InChannelLengths[C];
		const bool bEndsAtDims = Offset + Length == InDims;
		if (InChannelNames[C].IsNone() || Offset < 0 || Length <= 0 ||
			Offset % Grid != 0 || (Length % Grid != 0 && !bEndsAtDims) ||
			Offset < PrevEnd || Offset + Length > InDims)
		{
			OutError = FString::Printf(
				TEXT("channel %d ('%s') violates the channel rules"),
				C, *InChannelNames[C].ToString());
			return false;
		}
		UniqueNames.Add(InChannelNames[C]);
		PrevEnd = Offset + Length;
	}
	if (UniqueNames.Num() != ChannelCount)
	{
		OutError = TEXT("channel names are not unique");
		return false;
	}
	ChannelNames = TArray<FName>(InChannelNames.GetData(), ChannelCount);
	ChannelOffsets = TArray<int32>(InChannelOffsets.GetData(), ChannelCount);
	ChannelLengths = TArray<int32>(InChannelLengths.GetData(), ChannelCount);

	SchemaVersion = 2;
	Dims = InDims;
	PaddedDims = Pd;
	Count = InCount;
	Quantization = InQuantization;
	Metric = InMetric;
	Ids.Reset();
	SourceHash = InSourceHash;
	RecallAt10 = -1.0f;
	RecallSeed = 0;
	CrossDeviceRecallAt10 = -1.0f;
	IndexBlockVersion = 0;
	IndexBlockData.Reset();
	RebuildChannelTable();

	// Cosine channel banks carry per-row inverse sub-norms. The core Freeze already
	// re-derived them over the compacted rows, so ACCEPT them here (do not recompute) —
	// require the exact count so a bad graduation is a defined reject. Dot/L2 channel
	// banks carry none.
	if (InMetric == ESuperFAISSBankMetric::Cosine && InCount > 0)
	{
		const int64 Expected = static_cast<int64>(InCount) * ChannelCount;
		if (InChannelInvNorms.Num() != Expected)
		{
			OutError = FString::Printf(
				TEXT("channel sub-norm count %d != expected %lld"),
				InChannelInvNorms.Num(), static_cast<long long>(Expected));
			return false;
		}
		ChannelInvNorms = TArray<float>(InChannelInvNorms.GetData(), InChannelInvNorms.Num());
	}

	return ValidateContent(OutError);
}

void USuperFAISSVectorBank::RebuildChannelTable()
{
	ChannelTable.Reset();
	// A corrupted or tampered asset can carry mismatched parallel arrays; indexing
	// Offsets/Lengths by Names.Num() would read out of bounds DURING LOAD (Poirot
	// R-1a). Leave the table empty here; ValidateContent rejects the bank.
	if (ChannelOffsets.Num() != ChannelNames.Num() ||
		ChannelLengths.Num() != ChannelNames.Num())
	{
		return;
	}
	for (int32 C = 0; C < ChannelNames.Num(); ++C)
	{
		superfaiss::ChannelInfo Info;
		Info.offset = ChannelOffsets[C];
		// A channel declared to end at dims extends across the zero pad lanes so its
		// stored range stays on the element grid (pads contribute nothing).
		Info.length = (ChannelOffsets[C] + ChannelLengths[C] == Dims)
			? PaddedDims - ChannelOffsets[C]
			: ChannelLengths[C];
		ChannelTable.Add(Info);
	}
}

int32 USuperFAISSVectorBank::PaddedDims_Internal(int32 InDims, superfaiss::Quantization Q)
{
	return superfaiss::PaddedDims(InDims, Q);
}

FName USuperFAISSVectorBank::GetIdForIndex(int32 Index) const
{
	if (Ids.IsValidIndex(Index))
	{
		return Ids[Index];
	}
	// Id-less banks: NAME_None — the index is already on the hit, and minting FNames
	// here would intern a permanent table entry per distinct index (Poirot M1).
	return NAME_None;
}

int32 USuperFAISSVectorBank::GetIndexForId(FName Id) const
{
	if (Id.IsNone())
	{
		return INDEX_NONE;
	}
	return Ids.IndexOfByKey(Id);
}

bool USuperFAISSVectorBank::GetRowDequantized(int32 Row, TArray<float>& OutValues) const
{
	OutValues.Reset();
	if (!bValidated || Row < 0 || Row >= Count || Dims <= 0)
	{
		return false;
	}
	OutValues.SetNumUninitialized(Dims);
	const int64 RowStart = static_cast<int64>(Row) * PaddedDims;
	if (Quantization == ESuperFAISSBankQuantization::Int8)
	{
		// q * per-row scale - the exact inverse of QuantizeRowsInt8. Pad lanes
		// past Dims are dropped; a zero row has scale 0 and dequantizes to zero.
		const int8_t* Row8 =
			reinterpret_cast<const int8_t*>(Payload.GetData()) + RowStart;
		const float Scale = Scales.IsValidIndex(Row) ? Scales[Row] : 0.0f;
		for (int32 D = 0; D < Dims; ++D)
		{
			OutValues[D] = static_cast<float>(Row8[D]) * Scale;
		}
	}
	else
	{
		const float* RowF =
			reinterpret_cast<const float*>(Payload.GetData()) + RowStart;
		for (int32 D = 0; D < Dims; ++D)
		{
			OutValues[D] = RowF[D];
		}
	}
	return true;
}

superfaiss::BankView USuperFAISSVectorBank::GetBankView() const
{
	// Shipping-safe gate (Poirot S3): check() compiles out in Shipping, where a
	// PostLoad-rejected bank (corrupted pak, schema mismatch) must still never reach
	// the kernels. An invalid bank yields an empty view: every query returns no hits.
	if (!ensureMsgf(bValidated, TEXT("GetBankView on invalid bank %s"), *GetPathName()))
	{
		return superfaiss::BankView();
	}
	superfaiss::BankView View;
	View.rows = Payload.GetData();
	View.scales = Quantization == ESuperFAISSBankQuantization::Int8 ? Scales.GetData() : nullptr;
	View.count = Count;
	View.dims = Dims;
	View.paddedDims = PaddedDims;
	View.quant = ToCore(Quantization);
	View.metric = ToCore(Metric);
	if (ChannelTable.Num() > 0)
	{
		View.channels = ChannelTable.GetData();
		View.channelCount = ChannelTable.Num();
		View.channelInvNorms =
			ChannelInvNorms.Num() > 0 ? ChannelInvNorms.GetData() : nullptr;
	}
	return View;
}

void USuperFAISSVectorBank::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);

	Payload.BulkSerialize(Ar);
	Ar << Scales;
	Ar << IndexBlockVersion;
	Ar << IndexBlockData;
	// v2 sub-block: inverse sub-norms ride behind the reserved index block, gated on
	// the asset SchemaVersion so v1 archives are byte-identical to before.
	if (SchemaVersion >= 2)
	{
		Ar << ChannelInvNorms;
	}
	if (Ar.IsLoading())
	{
		RebuildChannelTable();
	}
}

void USuperFAISSVectorBank::PostLoad()
{
	Super::PostLoad();

	FString Error;
	// Accept schema 1 (channel-less) and 2 (channels); reject anything newer or
	// nonsensical - reject-over-degrade (T-044 N3).
	if (SchemaVersion < 1 || SchemaVersion > kMaxAssetSchemaVersion)
	{
		// Hard rejection: a version-mismatched bank never validates (plan §7).
		bValidated = false;
		UE_LOG(LogTemp, Error,
			TEXT("SuperFAISSVectorBank %s: schema version %d, expected 1..%d — bank rejected"),
			*GetPathName(), SchemaVersion, kMaxAssetSchemaVersion);
		return;
	}
	if (!ValidateContent(Error))
	{
		UE_LOG(LogTemp, Error, TEXT("SuperFAISSVectorBank %s: %s — bank rejected"),
			*GetPathName(), *Error);
	}
}

bool USuperFAISSVectorBank::ValidateContent(FString& OutError)
{
	using namespace superfaiss;

	bValidated = false;

	if (Ids.Num() != 0 && Ids.Num() != Count)
	{
		OutError = TEXT("id map size disagrees with count");
		return false;
	}
	const int64 ExpectedBytes =
		static_cast<int64>(Count) * PaddedDims * ElementSize(ToCore(Quantization));
	if (Payload.Num() != ExpectedBytes)
	{
		OutError = FString::Printf(TEXT("payload is %lld bytes, header implies %lld"),
			static_cast<long long>(Payload.Num()), static_cast<long long>(ExpectedBytes));
		return false;
	}
	const int32 ExpectedScales = Quantization == ESuperFAISSBankQuantization::Int8 ? Count : 0;
	if (Scales.Num() != ExpectedScales)
	{
		OutError = TEXT("scale array size disagrees with count/quantization");
		return false;
	}

	// The v2 channel block validates like everything else - "a bank that fails
	// never yields a view" was not true for it before this (Poirot R-1). The
	// parallel arrays must agree (R-1a; RebuildChannelTable already refused to
	// build a table from mismatched arrays), the inverse-norm array must be
	// exactly Count x ChannelCount on Cosine channel banks (R-1b; a truncated
	// array would send the kernels past the buffer mid-scan), and the view below
	// carries the channel table so the core's own channel rules run - including
	// "Cosine channels require invNorms" (R-1c; a missing array silently scored
	// without per-channel normalization before).
	const int32 ChannelCount = ChannelNames.Num();
	if (ChannelOffsets.Num() != ChannelCount || ChannelLengths.Num() != ChannelCount)
	{
		OutError = FString::Printf(TEXT("channel arrays disagree (%d names, %d offsets, %d lengths)"),
			ChannelCount, ChannelOffsets.Num(), ChannelLengths.Num());
		return false;
	}
	if (ChannelCount > 0)
	{
		if (ChannelTable.Num() != ChannelCount)
		{
			OutError = TEXT("channel table failed to build");
			return false;
		}
		const int64 ExpectedNorms = Metric == ESuperFAISSBankMetric::Cosine
			? static_cast<int64>(Count) * ChannelCount
			: 0;
		if (ChannelInvNorms.Num() != ExpectedNorms)
		{
			OutError = FString::Printf(
				TEXT("channel inverse-norm array is %d entries, bank requires %lld"),
				ChannelInvNorms.Num(), static_cast<long long>(ExpectedNorms));
			return false;
		}
	}
	else if (ChannelInvNorms.Num() != 0)
	{
		OutError = TEXT("inverse-norm array on a channel-less bank");
		return false;
	}

	BankView View;
	View.rows = Payload.GetData();
	View.scales = Quantization == ESuperFAISSBankQuantization::Int8 ? Scales.GetData() : nullptr;
	View.count = Count;
	View.dims = Dims;
	View.paddedDims = PaddedDims;
	View.quant = ToCore(Quantization);
	View.metric = ToCore(Metric);
	if (ChannelCount > 0)
	{
		View.channels = ChannelTable.GetData();
		View.channelCount = ChannelTable.Num();
		View.channelInvNorms = ChannelInvNorms.Num() > 0 ? ChannelInvNorms.GetData() : nullptr;
	}

	int32_t BadRow = -1;
	const Status ContentStatus = Count > 0
		? ValidateBankData(View, &BadRow)
		: ValidateBank(View);
	if (ContentStatus != Status::Ok)
	{
		OutError = FString::Printf(TEXT("content validation failed (status %d, row %d)"),
			static_cast<int32>(ContentStatus), BadRow);
		return false;
	}

	bValidated = true;
	return true;
}
