#pragma once

#include "CoreMinimal.h"

class USuperFAISSVectorBank;
enum class ESuperFAISSBankQuantization : uint8;

// Sidecar-format import (plan §10): <name>.wvbank.json header + <name>.wvbank.bin
// float32 payload. The JSON is parsed here with the engine's Json module — the
// SuperFAISS core is deliberately parser-free.
struct SUPERFAISSUNREALEDITOR_API FSuperFAISSBankImport
{
	// Parses, validates, bakes (normalize/quantize per header metric and the requested
	// quantization), computes the seeded recall report, and stamps the source hash.
	// Returns null with a line-item OutError on any rejection; never a partial bank.
	static USuperFAISSVectorBank* Import(
		const FString& JsonPath,
		UObject* Outer,
		FName AssetName,
		ESuperFAISSBankQuantization Quantization,
		FString& OutError);

	// Hash of the sidecar pair (json + bin bytes); re-import with an unchanged hash is
	// a no-op for callers that compare against an existing asset's SourceHash.
	static bool ComputeSourceHash(const FString& JsonPath, FString& OutHash, FString& OutError);

	// Validates every USuperFAISSVectorBank currently loaded/discoverable; returns the
	// number of invalid banks and appends their paths to OutInvalid. The commandlet's
	// working half, callable from tests.
	static int32 ValidateLoadedBanks(TArray<FString>& OutInvalid);
};
