#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "superfaiss/types.h"

#include "SuperFAISSVectorBank.generated.h"

UENUM()
enum class ESuperFAISSBankQuantization : uint8
{
	Float32 = 0,
	Int8 = 1,
};

UENUM()
enum class ESuperFAISSBankMetric : uint8
{
	Dot = 0,
	Cosine = 1,
	L2 = 2,
};

// A baked, immutable embedding bank. The payload is row-major, padded to a 16-byte
// stride, and held in a 16-byte-aligned allocation; Cosine banks store pre-normalized
// rows. Content is validated at load with SuperFAISS ValidateBankData — a bank that
// fails never yields a view.
UCLASS(BlueprintType)
class SUPERFAISSUNREAL_API USuperFAISSVectorBank : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(VisibleAnywhere, Category = "Bank")
	int32 SchemaVersion = 0;

	// Highest asset schema this plugin build understands: 1 = channel-less V1,
	// 2 = named channels (+ inverse sub-norms). Anything outside [1, Max] is
	// hard-rejected at load - reject-over-degrade (T-044 N3).
	static constexpr int32 kMaxAssetSchemaVersion = 2;

	UPROPERTY(VisibleAnywhere, Category = "Bank")
	int32 Dims = 0;

	UPROPERTY(VisibleAnywhere, Category = "Bank")
	int32 PaddedDims = 0;

	UPROPERTY(VisibleAnywhere, Category = "Bank")
	int32 Count = 0;

	UPROPERTY(VisibleAnywhere, Category = "Bank")
	ESuperFAISSBankQuantization Quantization = ESuperFAISSBankQuantization::Int8;

	UPROPERTY(VisibleAnywhere, Category = "Bank")
	ESuperFAISSBankMetric Metric = ESuperFAISSBankMetric::Cosine;

	// Optional index->ID map; empty means IDs are indices. Bijective (enforced at build).
	UPROPERTY(VisibleAnywhere, Category = "Bank")
	TArray<FName> Ids;

	UPROPERTY(VisibleAnywhere, Category = "Bank")
	FString SourceHash;

	// Quantization quality, measured at import with a seeded, reproducible sample
	// (recall@10 of the baked bank vs its float32 source; plan §10 step 4). -1 = not
	// measured (Float32 banks are not quantized, so there is nothing to compare).
	UPROPERTY(VisibleAnywhere, Category = "Bank")
	float RecallAt10 = -1.0f;

	UPROPERTY(VisibleAnywhere, Category = "Bank")
	uint64 RecallSeed = 0;

	// Named channels (schemaVersion 2 banks): contiguous element ranges of the row,
	// as declared by the sidecar (dims space). Empty = a v1 channel-less bank. On
	// Cosine channel banks, per-row inverse sub-norms are baked from the QUANTIZED
	// payload at import (plan D-V2-1 / T-044 W2a) and queries against named channels
	// score true per-channel cosines.
	UPROPERTY(VisibleAnywhere, Category = "Bank")
	TArray<FName> ChannelNames;

	UPROPERTY(VisibleAnywhere, Category = "Bank")
	TArray<int32> ChannelOffsets;

	UPROPERTY(VisibleAnywhere, Category = "Bank")
	TArray<int32> ChannelLengths;

	// Per-channel recall@10 on int8 channel banks (T-044 W2b): the honest-budget
	// number per channel, seeded like RecallAt10. Empty when not applicable.
	UPROPERTY(VisibleAnywhere, Category = "Bank")
	TArray<float> ChannelRecallAt10;

	// Builds the bank from raw float32 source rows (count x dims, unpadded), running the
	// full SuperFAISS bake: source validation, normalization (Cosine), quantization or
	// padding, then content validation. Returns false (and leaves the bank empty) on any
	// failure. Editor/tests entry point.
	bool InitFromSource(
		TConstArrayView<float> SourceRows,
		int32 InCount,
		int32 InDims,
		ESuperFAISSBankMetric InMetric,
		ESuperFAISSBankQuantization InQuantization,
		TConstArrayView<FName> InIds,
		const FString& InSourceHash,
		FString& OutError,
		TConstArrayView<FName> InChannelNames = {},
		TConstArrayView<int32> InChannelOffsets = {},
		TConstArrayView<int32> InChannelLengths = {});

	// Builds the bank from an ALREADY-BAKED payload (padded rows + int8 scales) -
	// the scratch-bank Freeze path (plan section 7): rows were normalized and
	// quantized at append with the importer's own math, so copying them preserves
	// bit-identity with the scratch bank they came from. Channel-less, schema 1.
	// Content validation still runs (reject-over-degrade).
	bool InitFromBaked(
		const void* BakedRows,
		const float* BakedScales,
		int32 InCount,
		int32 InDims,
		ESuperFAISSBankMetric InMetric,
		ESuperFAISSBankQuantization InQuantization,
		const FString& InSourceHash,
		FString& OutError);

	UFUNCTION(BlueprintPure, Category = "Bank")
	int32 GetChannelCount() const { return ChannelNames.Num(); }

	// Channel index by name, or INDEX_NONE.
	UFUNCTION(BlueprintPure, Category = "Bank")
	int32 GetChannelIndex(FName Name) const { return ChannelNames.IndexOfByKey(Name); }

	// True if the loaded/built payload passed structural + content validation.
	UFUNCTION(BlueprintPure, Category = "Bank")
	bool IsValid() const { return bValidated; }

	UFUNCTION(BlueprintPure, Category = "Bank")
	int64 GetPayloadBytes() const { return static_cast<int64>(Payload.Num()); }

	UFUNCTION(BlueprintPure, Category = "Bank")
	FName GetIdForIndex(int32 Index) const;

	// Reverse lookup: the row index carrying Id, or INDEX_NONE (also for id-less
	// banks). Linear scan — editor-tool and setup-time speed, not per-frame.
	UFUNCTION(BlueprintPure, Category = "Bank")
	int32 GetIndexForId(FName Id) const;

	// Non-owning view for the query path. Asserts IsValid().
	superfaiss::BankView GetBankView() const;

	//~ UObject
	virtual void Serialize(FArchive& Ar) override;
	virtual void PostLoad() override;

private:
	bool ValidateContent(FString& OutError);
	void RebuildChannelTable();

	// Cosine channel banks: count x ChannelNames.Num() inverse sub-norms, baked from
	// the quantized payload. Serialized manually beside Scales.
	TArray<float> ChannelInvNorms;
	// Transient core-facing channel table (padded-space ranges), rebuilt on load.
	TArray<superfaiss::ChannelInfo> ChannelTable;

public:
	// Read access for import tooling (per-channel recall, linter floor).
	TConstArrayView<float> GetChannelInvNorms() const { return ChannelInvNorms; }

private:

	// The PaddedDims member shadows superfaiss::PaddedDims inside class scope.
	static int32 PaddedDims_Internal(int32 InDims, superfaiss::Quantization Q);

	// Row payload; 16-byte-aligned allocator so kernels' alignment contract holds
	// directly on the loaded data. Not a UPROPERTY: serialized manually in Serialize().
	TArray<uint8, TAlignedHeapAllocator<16>> Payload;

	// Int8 per-row dequantization scales; empty for Float32 banks.
	TArray<float> Scales;

	// Reserved index block (plan §7): versioned, opaque, ignored when unknown — V2 ANN
	// index data lands here without a format break.
	int32 IndexBlockVersion = 0;
	TArray<uint8> IndexBlockData;

	bool bValidated = false;
};
