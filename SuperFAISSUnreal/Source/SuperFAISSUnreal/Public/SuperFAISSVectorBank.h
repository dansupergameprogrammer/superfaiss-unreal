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
		FString& OutError);

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
