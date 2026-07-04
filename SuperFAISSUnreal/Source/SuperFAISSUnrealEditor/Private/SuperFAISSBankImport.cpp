#include "SuperFAISSBankImport.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UObjectIterator.h"
#include "SuperFAISSVectorBank.h"

#include "superfaiss/superfaiss.h"

namespace
{
	bool ParseMetric(const FString& Text, ESuperFAISSBankMetric& Out)
	{
		if (Text == TEXT("dot")) { Out = ESuperFAISSBankMetric::Dot; return true; }
		if (Text == TEXT("cosine")) { Out = ESuperFAISSBankMetric::Cosine; return true; }
		if (Text == TEXT("l2")) { Out = ESuperFAISSBankMetric::L2; return true; }
		return false;
	}

	FString BinPathFor(const FString& JsonPath)
	{
		// <name>.wvbank.json -> <name>.wvbank.bin
		FString Path = JsonPath;
		if (Path.EndsWith(TEXT(".json")))
		{
			Path.LeftChopInline(5);
		}
		return Path + TEXT(".bin");
	}

	// Per-channel seeded recall@10 (T-044 W2b): the same self-query sampling,
	// restricted to one channel on both views - the float reference carries its own
	// channel inverse sub-norms so both sides rank by true per-channel cosine. The
	// honest-budget number for weak channels on int8 banks.
	float ComputeChannelRecallAt10(
		const TArray<float>& NormalizedRows,
		int32 Count,
		int32 Dims,
		USuperFAISSVectorBank* BakedBank,
		uint64 Seed,
		int32 ChannelIndex)
	{
		using namespace superfaiss;

		if (Count < 2)
		{
			return 1.0f;
		}

		const int32 RefPd = PaddedDims(Dims, Quantization::Float32);
		TArray<float, TAlignedHeapAllocator<16>> RefPayload;
		RefPayload.SetNumZeroed(Count * RefPd);
		PadRowsFloat32(NormalizedRows.GetData(), Count, Dims, RefPd, RefPayload.GetData());

		// Channel tables in each view's padded space (declared dims-space ranges;
		// a channel ending at dims extends across that view's pad lanes).
		TArray<ChannelInfo> RefChannels;
		for (int32 C = 0; C < BakedBank->GetChannelCount(); ++C)
		{
			ChannelInfo Info;
			Info.offset = BakedBank->ChannelOffsets[C];
			Info.length =
				(BakedBank->ChannelOffsets[C] + BakedBank->ChannelLengths[C] == Dims)
					? RefPd - BakedBank->ChannelOffsets[C]
					: BakedBank->ChannelLengths[C];
			RefChannels.Add(Info);
		}

		BankView RefView;
		RefView.rows = RefPayload.GetData();
		RefView.count = Count;
		RefView.dims = Dims;
		RefView.paddedDims = RefPd;
		RefView.quant = Quantization::Float32;
		RefView.metric = Metric::Cosine;
		RefView.channels = RefChannels.GetData();
		RefView.channelCount = RefChannels.Num();
		TArray<float> RefInvNorms;
		RefInvNorms.SetNumZeroed(static_cast<int64>(Count) * RefChannels.Num());
		if (ComputeChannelInverseNorms(RefView, RefInvNorms.GetData()) != Status::Ok)
		{
			return -1.0f;
		}
		RefView.channelInvNorms = RefInvNorms.GetData();

		const BankView BakedView = BakedBank->GetBankView();

		const int32 SampleCount = FMath::Min(500, Count);
		const int32 K = FMath::Min(10, Count - 1);

		TArray<float, TAlignedHeapAllocator<16>> RefQuery;
		RefQuery.SetNumZeroed(RefPd);
		TArray<float, TAlignedHeapAllocator<16>> BakedQuery;
		BakedQuery.SetNumZeroed(BakedView.paddedDims);
		Workspace RefWs, BakedWs;
		TArray<Hit> RefHits, BakedHits;
		RefHits.SetNumUninitialized(K);
		BakedHits.SetNumUninitialized(K);
		TArray<uint32> Exclude;
		Exclude.SetNumZeroed((Count + 31) / 32);

		QuerySegment RefSeg;
		RefSeg.offset = RefChannels[ChannelIndex].offset;
		RefSeg.length = RefChannels[ChannelIndex].length;
		RefSeg.weight = 1.0f;
		QuerySegment BakedSeg;
		BakedSeg.offset = BakedView.channels[ChannelIndex].offset;
		BakedSeg.length = BakedView.channels[ChannelIndex].length;
		BakedSeg.weight = 1.0f;

		uint64 State = Seed ? Seed : 1;
		int32 TotalHits = 0;
		int32 TotalPossible = 0;
		for (int32 S = 0; S < SampleCount; ++S)
		{
			State ^= State >> 12;
			State ^= State << 25;
			State ^= State >> 27;
			const int32 Row = static_cast<int32>((State * 0x2545F4914F6CDD1Dull) % Count);

			// Skip rows whose channel is zero-norm on either side (the W3 query law
			// would reject them; the linter floor reports them separately).
			double SubNorm = 0.0;
			for (int32 J = BakedBank->ChannelOffsets[ChannelIndex];
				J < BakedBank->ChannelOffsets[ChannelIndex] +
					BakedBank->ChannelLengths[ChannelIndex]; ++J)
			{
				const float V = NormalizedRows[Row * Dims + J];
				SubNorm += static_cast<double>(V) * V;
			}
			if (SubNorm <= 0.0)
			{
				continue;
			}

			FMemory::Memzero(RefQuery.GetData(), RefPd * sizeof(float));
			FMemory::Memzero(BakedQuery.GetData(), BakedView.paddedDims * sizeof(float));
			FMemory::Memcpy(RefQuery.GetData(), NormalizedRows.GetData() + Row * Dims,
				Dims * sizeof(float));
			FMemory::Memcpy(BakedQuery.GetData(), NormalizedRows.GetData() + Row * Dims,
				Dims * sizeof(float));
			FMemory::Memzero(Exclude.GetData(), Exclude.Num() * sizeof(uint32));
			Exclude[Row >> 5] |= 1u << (Row & 31);

			QueryParams RefParams;
			RefParams.k = K;
			RefParams.excludeBits = Exclude.GetData();
			RefParams.segments = &RefSeg;
			RefParams.segmentCount = 1;
			QueryParams BakedParams = RefParams;
			BakedParams.segments = &BakedSeg;

			int32_t RefN = 0, BakedN = 0;
			if (Query(RefView, RefQuery.GetData(), RefParams, RefWs, RefHits.GetData(),
					&RefN) != Status::Ok ||
				Query(BakedView, BakedQuery.GetData(), BakedParams, BakedWs,
					BakedHits.GetData(), &BakedN) != Status::Ok)
			{
				continue;
			}
			TSet<int32> RefSet;
			for (int32 i = 0; i < RefN; ++i)
			{
				RefSet.Add(RefHits[i].index);
			}
			for (int32 i = 0; i < BakedN; ++i)
			{
				TotalHits += RefSet.Contains(BakedHits[i].index) ? 1 : 0;
			}
			TotalPossible += RefN;
		}
		return TotalPossible > 0
			? static_cast<float>(TotalHits) / static_cast<float>(TotalPossible)
			: 1.0f;
	}

	// Seeded recall@10: sample rows as self-queries, compare the baked (possibly
	// quantized) bank's top-10 against a float32 reference bank built from the same
	// normalized source. Reproducible: the seed is fixed and recorded on the asset.
	float ComputeRecallAt10(
		const TArray<float>& NormalizedRows,
		int32 Count,
		int32 Dims,
		ESuperFAISSBankMetric Metric,
		USuperFAISSVectorBank* BakedBank,
		uint64 Seed)
	{
		using namespace superfaiss;

		if (Count < 2)
		{
			return 1.0f;
		}

		// Float32 reference view over padded rows.
		const int32 RefPd = PaddedDims(Dims, Quantization::Float32);
		TArray<float, TAlignedHeapAllocator<16>> RefPayload;
		RefPayload.SetNumZeroed(Count * RefPd);
		PadRowsFloat32(NormalizedRows.GetData(), Count, Dims, RefPd, RefPayload.GetData());

		BankView RefView;
		RefView.rows = RefPayload.GetData();
		RefView.count = Count;
		RefView.dims = Dims;
		RefView.paddedDims = RefPd;
		RefView.quant = Quantization::Float32;
		RefView.metric = Metric == ESuperFAISSBankMetric::L2 ? Metric::L2
			: Metric == ESuperFAISSBankMetric::Dot ? Metric::Dot : Metric::Cosine;

		const BankView BakedView = BakedBank->GetBankView();

		const int32 SampleCount = FMath::Min(1000, Count);
		const int32 K = FMath::Min(10, Count - 1);

		TArray<float, TAlignedHeapAllocator<16>> RefQuery;
		RefQuery.SetNumZeroed(RefPd);
		TArray<float, TAlignedHeapAllocator<16>> BakedQuery;
		BakedQuery.SetNumZeroed(BakedView.paddedDims);

		Workspace RefWs;
		Workspace BakedWs;
		TArray<Hit> RefHits;
		RefHits.SetNumUninitialized(K);
		TArray<Hit> BakedHits;
		BakedHits.SetNumUninitialized(K);
		TArray<uint32> Exclude;
		Exclude.SetNumZeroed((Count + 31) / 32);

		uint64 State = Seed ? Seed : 1;
		int32 TotalHits = 0;
		int32 TotalPossible = 0;

		for (int32 S = 0; S < SampleCount; ++S)
		{
			State ^= State >> 12;
			State ^= State << 25;
			State ^= State >> 27;
			const int32 Row = static_cast<int32>((State * 0x2545F4914F6CDD1Dull) % Count);

			FMemory::Memzero(RefQuery.GetData(), RefPd * sizeof(float));
			FMemory::Memzero(BakedQuery.GetData(), BakedView.paddedDims * sizeof(float));
			FMemory::Memcpy(RefQuery.GetData(), NormalizedRows.GetData() + Row * Dims,
				Dims * sizeof(float));
			FMemory::Memcpy(BakedQuery.GetData(), NormalizedRows.GetData() + Row * Dims,
				Dims * sizeof(float));

			FMemory::Memzero(Exclude.GetData(), Exclude.Num() * sizeof(uint32));
			Exclude[Row >> 5] |= 1u << (Row & 31);

			QueryParams Params;
			Params.k = K;
			Params.excludeBits = Exclude.GetData();

			int32_t RefN = 0;
			int32_t BakedN = 0;
			if (Query(RefView, RefQuery.GetData(), Params, RefWs, RefHits.GetData(), &RefN) != Status::Ok ||
				Query(BakedView, BakedQuery.GetData(), Params, BakedWs, BakedHits.GetData(), &BakedN) != Status::Ok)
			{
				continue;
			}

			TSet<int32> RefSet;
			for (int32 i = 0; i < RefN; ++i)
			{
				RefSet.Add(RefHits[i].index);
			}
			for (int32 i = 0; i < BakedN; ++i)
			{
				TotalHits += RefSet.Contains(BakedHits[i].index) ? 1 : 0;
			}
			TotalPossible += RefN;
		}

		return TotalPossible > 0 ? static_cast<float>(TotalHits) / TotalPossible : 1.0f;
	}
}

USuperFAISSVectorBank* FSuperFAISSBankImport::Import(
	const FString& JsonPath,
	UObject* Outer,
	FName AssetName,
	ESuperFAISSBankQuantization Quantization,
	FString& OutError)
{
	OutError.Reset();

	// --- Header ---
	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *JsonPath))
	{
		OutError = FString::Printf(TEXT("cannot read %s"), *JsonPath);
		return nullptr;
	}
	TSharedPtr<FJsonObject> Header;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JsonText), Header) ||
		!Header.IsValid())
	{
		OutError = TEXT("header is not valid JSON");
		return nullptr;
	}

	int32 HeaderSchema = 0;
	int32 Dims = 0;
	int32 Count = 0;
	FString MetricText;
	FString DtypeText;
	if (!Header->TryGetNumberField(TEXT("schemaVersion"), HeaderSchema) ||
		!Header->TryGetNumberField(TEXT("dims"), Dims) ||
		!Header->TryGetNumberField(TEXT("count"), Count) ||
		!Header->TryGetStringField(TEXT("metric"), MetricText) ||
		!Header->TryGetStringField(TEXT("dtype"), DtypeText))
	{
		OutError = TEXT("header is missing a required field (schemaVersion/dims/count/metric/dtype)");
		return nullptr;
	}
	if (HeaderSchema < 1 || HeaderSchema > 2)
	{
		OutError = FString::Printf(
			TEXT("header schemaVersion %d, supported 1 (channel-less) or 2 (channels)"),
			HeaderSchema);
		return nullptr;
	}
	if (DtypeText != TEXT("float32"))
	{
		OutError = FString::Printf(TEXT("unsupported dtype '%s'"), *DtypeText);
		return nullptr;
	}
	ESuperFAISSBankMetric Metric;
	if (!ParseMetric(MetricText, Metric))
	{
		OutError = FString::Printf(TEXT("unknown metric '%s'"), *MetricText);
		return nullptr;
	}
	if (Dims <= 0 || Count < 0)
	{
		OutError = FString::Printf(TEXT("nonsensical dims/count (%d/%d)"), Dims, Count);
		return nullptr;
	}

	TArray<FName> Ids;
	const TArray<TSharedPtr<FJsonValue>>* IdValues = nullptr;
	if (Header->TryGetArrayField(TEXT("ids"), IdValues))
	{
		if (IdValues->Num() != Count)
		{
			OutError = FString::Printf(TEXT("%d ids for %d rows"), IdValues->Num(), Count);
			return nullptr;
		}
		Ids.Reserve(Count);
		for (const TSharedPtr<FJsonValue>& V : *IdValues)
		{
			FString IdText;
			if (!V.IsValid() || !V->TryGetString(IdText))
			{
				OutError = TEXT("non-string id in header");
				return nullptr;
			}
			Ids.Add(FName(*IdText));
		}
	}

	// --- Channels (schemaVersion 2) ---
	TArray<FName> ChannelNames;
	TArray<int32> ChannelOffsets;
	TArray<int32> ChannelLengths;
	const TArray<TSharedPtr<FJsonValue>>* ChannelValues = nullptr;
	if (Header->TryGetArrayField(TEXT("channels"), ChannelValues))
	{
		if (HeaderSchema < 2)
		{
			OutError = TEXT("channels require schemaVersion 2");
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& V : *ChannelValues)
		{
			const TSharedPtr<FJsonObject>* ChannelObject = nullptr;
			FString Name;
			int32 Offset = -1;
			int32 Length = -1;
			if (!V.IsValid() || !V->TryGetObject(ChannelObject) ||
				!(*ChannelObject)->TryGetStringField(TEXT("name"), Name) ||
				!(*ChannelObject)->TryGetNumberField(TEXT("offset"), Offset) ||
				!(*ChannelObject)->TryGetNumberField(TEXT("length"), Length) ||
				Name.IsEmpty())
			{
				OutError = TEXT("malformed channel entry (name/offset/length required)");
				return nullptr;
			}
			ChannelNames.Add(FName(*Name));
			ChannelOffsets.Add(Offset);
			ChannelLengths.Add(Length);
		}
		// Range/grid/order/uniqueness rules are enforced by InitFromSource with
		// line-item errors - one validator, one truth.
	}
	else if (HeaderSchema >= 2)
	{
		OutError = TEXT("schemaVersion 2 without a channels table (declare 1 instead)");
		return nullptr;
	}

	// --- Payload ---
	TArray<uint8> BinBytes;
	const FString BinPath = BinPathFor(JsonPath);
	if (!FFileHelper::LoadFileToArray(BinBytes, *BinPath))
	{
		OutError = FString::Printf(TEXT("cannot read %s"), *BinPath);
		return nullptr;
	}
	const int64 ExpectedBytes = static_cast<int64>(Count) * Dims * sizeof(float);
	if (BinBytes.Num() != ExpectedBytes)
	{
		OutError = FString::Printf(TEXT("payload is %d bytes, header implies %lld"),
			BinBytes.Num(), static_cast<long long>(ExpectedBytes));
		return nullptr;
	}

	TArray<float> Rows;
	Rows.SetNumUninitialized(Count * Dims);
	FMemory::Memcpy(Rows.GetData(), BinBytes.GetData(), ExpectedBytes);

	// --- Hash, bake, recall ---
	FString Hash;
	if (!ComputeSourceHash(JsonPath, Hash, OutError))
	{
		return nullptr;
	}

	USuperFAISSVectorBank* Bank = NewObject<USuperFAISSVectorBank>(
		Outer ? Outer : GetTransientPackage(), AssetName, RF_Public | RF_Standalone);
	if (!Bank->InitFromSource(Rows, Count, Dims, Metric, Quantization, Ids, Hash, OutError,
			ChannelNames, ChannelOffsets, ChannelLengths))
	{
		Bank->ClearFlags(RF_Public | RF_Standalone);
		Bank->MarkAsGarbage();
		return nullptr;
	}

	// Recall is measured against the normalized source InitFromSource baked from;
	// normalize a copy the same way for the reference.
	if (Quantization == ESuperFAISSBankQuantization::Int8 && Count > 1)
	{
		if (Metric == ESuperFAISSBankMetric::Cosine)
		{
			int32_t BadRow = -1;
			superfaiss::NormalizeRows(Rows.GetData(), Count, Dims, &BadRow);
		}
		constexpr uint64 kRecallSeed = 0x5EEDF00DCAFEBEEFull;
		Bank->RecallAt10 = ComputeRecallAt10(Rows, Count, Dims, Metric, Bank, kRecallSeed);
		Bank->RecallSeed = kRecallSeed;

		// Per-channel recall on int8 Cosine channel banks (T-044 W2b): the
		// honest-budget number per channel, same seed discipline.
		if (Bank->GetChannelCount() > 0 && Metric == ESuperFAISSBankMetric::Cosine)
		{
			Bank->ChannelRecallAt10.Reset();
			for (int32 C = 0; C < Bank->GetChannelCount(); ++C)
			{
				Bank->ChannelRecallAt10.Add(ComputeChannelRecallAt10(
					Rows, Count, Dims, Bank, kRecallSeed + 1 + C, C));
			}
		}
		UE_LOG(LogTemp, Display,
			TEXT("SuperFAISSBankImport %s: %d x %d %s int8, recall@10 %.4f (seed %llx)"),
			*AssetName.ToString(), Count, Dims, *MetricText, Bank->RecallAt10, Bank->RecallSeed);
	}

	return Bank;
}

bool FSuperFAISSBankImport::ComputeSourceHash(const FString& JsonPath, FString& OutHash, FString& OutError)
{
	TArray<uint8> JsonBytes;
	TArray<uint8> BinBytes;
	if (!FFileHelper::LoadFileToArray(JsonBytes, *JsonPath) ||
		!FFileHelper::LoadFileToArray(BinBytes, *BinPathFor(JsonPath)))
	{
		OutError = TEXT("cannot read sidecar pair for hashing");
		return false;
	}
	FSHA1 Sha;
	Sha.Update(JsonBytes.GetData(), JsonBytes.Num());
	Sha.Update(BinBytes.GetData(), BinBytes.Num());
	Sha.Final();
	uint8 Digest[FSHA1::DigestSize];
	Sha.GetHash(Digest);
	OutHash = BytesToHex(Digest, FSHA1::DigestSize);
	return true;
}

int32 FSuperFAISSBankImport::ValidateLoadedBanks(TArray<FString>& OutInvalid)
{
	int32 Invalid = 0;
	for (TObjectIterator<USuperFAISSVectorBank> It; It; ++It)
	{
		if (!It->IsValid())
		{
			++Invalid;
			OutInvalid.Add(It->GetPathName());
		}
	}
	return Invalid;
}
