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
	FString& OutError)
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

	SchemaVersion = kSchemaVersion;
	Dims = InDims;
	PaddedDims = Pd;
	Count = InCount;
	Quantization = InQuantization;
	Metric = InMetric;
	Ids = TArray<FName>(InIds.GetData(), InIds.Num());
	SourceHash = InSourceHash;
	IndexBlockVersion = 0;
	IndexBlockData.Reset();

	return ValidateContent(OutError);
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
	return View;
}

void USuperFAISSVectorBank::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);

	Payload.BulkSerialize(Ar);
	Ar << Scales;
	Ar << IndexBlockVersion;
	Ar << IndexBlockData;
}

void USuperFAISSVectorBank::PostLoad()
{
	Super::PostLoad();

	FString Error;
	if (SchemaVersion != superfaiss::kSchemaVersion)
	{
		// Hard rejection: a version-mismatched bank never validates (plan §7).
		bValidated = false;
		UE_LOG(LogTemp, Error,
			TEXT("SuperFAISSVectorBank %s: schema version %d, expected %d — bank rejected"),
			*GetPathName(), SchemaVersion, superfaiss::kSchemaVersion);
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

	BankView View;
	View.rows = Payload.GetData();
	View.scales = Quantization == ESuperFAISSBankQuantization::Int8 ? Scales.GetData() : nullptr;
	View.count = Count;
	View.dims = Dims;
	View.paddedDims = PaddedDims;
	View.quant = ToCore(Quantization);
	View.metric = ToCore(Metric);

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
