#include "SuperFAISSToolset.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SuperFAISSBankImport.h"
#include "SuperFAISSBankLint.h"
#include "SuperFAISSPrototypeAsset.h"
#include "SuperFAISSScratchBank.h"
#include "UObject/UObjectIterator.h"
#include "SuperFAISSSubsystem.h"
#include "SuperFAISSVectorBank.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	FString ToJson(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
		return Out;
	}

	FString JsonError(const FString& Message)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("error"), Message);
		return ToJson(Object);
	}

	const TCHAR* MetricName(ESuperFAISSBankMetric Metric)
	{
		switch (Metric)
		{
		case ESuperFAISSBankMetric::Dot: return TEXT("Dot");
		case ESuperFAISSBankMetric::Cosine: return TEXT("Cosine");
		default: return TEXT("L2");
		}
	}

	USuperFAISSVectorBank* LoadBank(const FString& BankPath, FString& OutError)
	{
		USuperFAISSVectorBank* Bank =
			LoadObject<USuperFAISSVectorBank>(nullptr, *BankPath);
		if (Bank == nullptr)
		{
			OutError = FString::Printf(TEXT("no bank at %s"), *BankPath);
			return nullptr;
		}
		if (!Bank->IsValid())
		{
			OutError = FString::Printf(TEXT("bank failed validation: %s"), *BankPath);
			return nullptr;
		}
		return Bank;
	}

	TSharedRef<FJsonObject> BankSummary(const USuperFAISSVectorBank& Bank)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("path"), Bank.GetPathName());
		Object->SetNumberField(TEXT("count"), Bank.Count);
		Object->SetNumberField(TEXT("dims"), Bank.Dims);
		Object->SetStringField(TEXT("metric"), MetricName(Bank.Metric));
		Object->SetStringField(TEXT("quantization"),
			Bank.Quantization == ESuperFAISSBankQuantization::Int8 ? TEXT("Int8")
			                                                       : TEXT("Float32"));
		Object->SetNumberField(TEXT("payloadBytes"),
			static_cast<double>(Bank.GetPayloadBytes()));
		return Object;
	}

	FString HitsResult(const USuperFAISSVectorBank& Bank,
		const TArray<FSuperFAISSHit>& Hits)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("bank"), Bank.GetPathName());
		TArray<TSharedPtr<FJsonValue>> HitValues;
		for (const FSuperFAISSHit& Hit : Hits)
		{
			const TSharedRef<FJsonObject> HitObject = MakeShared<FJsonObject>();
			HitObject->SetNumberField(TEXT("index"), Hit.Index);
			if (!Hit.Id.IsNone())
			{
				HitObject->SetStringField(TEXT("id"), Hit.Id.ToString());
			}
			HitObject->SetNumberField(TEXT("score"), Hit.Score);
			HitObject->SetNumberField(TEXT("margin"), Hit.Margin);
			HitValues.Add(MakeShared<FJsonValueObject>(HitObject));
		}
		Object->SetArrayField(TEXT("hits"), HitValues);
		return ToJson(Object);
	}

	FString RunBankQuery(const USuperFAISSVectorBank* Bank, const TArray<float>& Query,
		int32 K, bool bScoreAsDot)
	{
		USuperFAISSSubsystem* Subsystem =
			GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
		if (Subsystem == nullptr)
		{
			return JsonError(TEXT("subsystem unavailable"));
		}
		FSuperFAISSQueryArgs Args;
		Args.K = FMath::Clamp(K, 1, 1000);
		Args.bScoreAsDot = bScoreAsDot;
		TArray<FSuperFAISSHit> Hits;
		if (!Subsystem->QuerySync(Bank, Query, Args, Hits))
		{
			return JsonError(TEXT("query rejected (dims mismatch or invalid vector)"));
		}
		return HitsResult(*Bank, Hits);
	}
}

FString USuperFAISSToolset::Echo(const FString& Message)
{
	return FString::Printf(TEXT("echo: %s (thread: %s)"), *Message,
		IsInGameThread() ? TEXT("game") : TEXT("worker"));
}

FString USuperFAISSToolset::ListBanks()
{
	FAssetRegistryModule& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	AssetRegistry.Get().SearchAllAssets(/*bSynchronousSearch*/ true);
	TArray<FAssetData> Assets;
	AssetRegistry.Get().GetAssetsByClass(
		USuperFAISSVectorBank::StaticClass()->GetClassPathName(), Assets);

	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> BankValues;
	for (const FAssetData& Asset : Assets)
	{
		if (const USuperFAISSVectorBank* Bank =
			Cast<USuperFAISSVectorBank>(Asset.GetAsset()))
		{
			BankValues.Add(MakeShared<FJsonValueObject>(BankSummary(*Bank)));
		}
	}
	Object->SetNumberField(TEXT("count"), BankValues.Num());
	Object->SetArrayField(TEXT("banks"), BankValues);
	return ToJson(Object);
}

FString USuperFAISSToolset::DescribeBank(const FString& BankPath)
{
	FString Error;
	const USuperFAISSVectorBank* Bank = LoadBank(BankPath, Error);
	if (Bank == nullptr)
	{
		return JsonError(Error);
	}
	const TSharedRef<FJsonObject> Object = BankSummary(*Bank);
	Object->SetNumberField(TEXT("schemaVersion"), Bank->SchemaVersion);
	Object->SetNumberField(TEXT("paddedDims"), Bank->PaddedDims);
	Object->SetBoolField(TEXT("hasIds"), Bank->Ids.Num() > 0);
	Object->SetNumberField(TEXT("recallAt10"), Bank->RecallAt10);
	Object->SetNumberField(TEXT("crossDeviceRecallAt10"), Bank->CrossDeviceRecallAt10);
	Object->SetStringField(TEXT("sourceHash"), Bank->SourceHash);
	if (Bank->GetChannelCount() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Channels;
		for (int32 C = 0; C < Bank->GetChannelCount(); ++C)
		{
			const TSharedRef<FJsonObject> Channel = MakeShared<FJsonObject>();
			Channel->SetStringField(TEXT("name"), Bank->ChannelNames[C].ToString());
			Channel->SetNumberField(TEXT("offset"), Bank->ChannelOffsets[C]);
			Channel->SetNumberField(TEXT("dims"), Bank->ChannelLengths[C]);
			if (Bank->ChannelRecallAt10.IsValidIndex(C))
			{
				Channel->SetNumberField(TEXT("recallAt10"), Bank->ChannelRecallAt10[C]);
			}
			Channels.Add(MakeShared<FJsonValueObject>(Channel));
		}
		Object->SetArrayField(TEXT("channels"), Channels);
	}
	return ToJson(Object);
}

FString USuperFAISSToolset::QueryBank(const FString& BankPath, const FString& RowId,
	int32 RowIndex, const TArray<float>& Vector, const TArray<FString>& ChannelNames,
	const TArray<float>& ChannelWeights, const TArray<int32>& BiasIndices,
	const TArray<float>& BiasValues, int32 K, bool bScoreAsDot, bool bCrossDeviceExact)
{
	FString Error;
	USuperFAISSVectorBank* Bank = LoadBank(BankPath, Error);
	if (Bank == nullptr)
	{
		return JsonError(Error);
	}

	const int32 Sources =
		(!RowId.IsEmpty() ? 1 : 0) + (RowIndex >= 0 ? 1 : 0) + (Vector.Num() > 0 ? 1 : 0);
	if (Sources != 1)
	{
		return JsonError(TEXT("provide exactly one of RowId, RowIndex, Vector"));
	}

	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (Subsystem == nullptr)
	{
		return JsonError(TEXT("subsystem unavailable"));
	}

	TArray<float> Query;
	if (Vector.Num() > 0)
	{
		if (Vector.Num() != Bank->Dims)
		{
			return JsonError(FString::Printf(
				TEXT("vector has %d dims; bank has %d"), Vector.Num(), Bank->Dims));
		}
		Query = Vector;
	}
	else
	{
		int32 Row = RowIndex;
		if (!RowId.IsEmpty())
		{
			if (Bank->Ids.Num() == 0)
			{
				return JsonError(TEXT("bank carries no ids; query by RowIndex or Vector"));
			}
			Row = Bank->GetIndexForId(FName(*RowId));
			if (Row == INDEX_NONE)
			{
				return JsonError(FString::Printf(TEXT("id not in bank: %s"), *RowId));
			}
		}
		if (Row < 0 || Row >= Bank->Count)
		{
			return JsonError(FString::Printf(TEXT("row out of range: %d"), Row));
		}
		if (!Subsystem->MakeCentroidQuery(Bank, {Row}, Query))
		{
			return JsonError(TEXT("row extraction failed"));
		}
	}
	if (ChannelNames.Num() != ChannelWeights.Num())
	{
		return JsonError(TEXT("ChannelNames and ChannelWeights must be parallel arrays"));
	}
	if (BiasIndices.Num() != BiasValues.Num())
	{
		return JsonError(TEXT("BiasIndices and BiasValues must be parallel arrays"));
	}
	USuperFAISSSubsystem* QuerySubsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	FSuperFAISSQueryArgs Args;
	Args.K = FMath::Clamp(K, 1, 1000);
	Args.bScoreAsDot = bScoreAsDot;
	Args.bCrossDeviceExact = bCrossDeviceExact;
	for (int32 C = 0; C < ChannelNames.Num(); ++C)
	{
		Args.Channels.Add({FName(*ChannelNames[C]), ChannelWeights[C]});
	}
	for (int32 B = 0; B < BiasIndices.Num(); ++B)
	{
		FSuperFAISSBiasPair Pair;
		Pair.Index = BiasIndices[B];
		Pair.Bias = BiasValues[B];
		Args.BiasPairs.Add(Pair);
	}
	TArray<FSuperFAISSHit> Hits;
	if (!QuerySubsystem->QuerySync(Bank, Query, Args, Hits))
	{
		return JsonError(TEXT(
			"query rejected (dims mismatch, unknown channel, bad bias pair, invalid "
			"vector, or cross-device mode on a non-int8 bank)"));
	}
	return HitsResult(*Bank, Hits);
}

FString USuperFAISSToolset::QueryPrototype(const FString& BankPath,
	const TArray<FString>& RowIds, const TArray<int32>& RowIndices,
	const FString& PrototypeAssetPath, int32 K)
{
	FString Error;
	USuperFAISSVectorBank* Bank = LoadBank(BankPath, Error);
	if (Bank == nullptr)
	{
		return JsonError(Error);
	}
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (Subsystem == nullptr)
	{
		return JsonError(TEXT("subsystem unavailable"));
	}

	TArray<float> Query;
	if (!PrototypeAssetPath.IsEmpty())
	{
		USuperFAISSPrototypeAsset* Prototype =
			LoadObject<USuperFAISSPrototypeAsset>(nullptr, *PrototypeAssetPath);
		if (Prototype == nullptr)
		{
			return JsonError(FString::Printf(
				TEXT("no prototype asset at %s"), *PrototypeAssetPath));
		}
		if (Prototype->Query.Num() != Bank->Dims)
		{
			return JsonError(FString::Printf(TEXT("prototype has %d dims; bank has %d"),
				Prototype->Query.Num(), Bank->Dims));
		}
		Query = Prototype->Query;
	}
	else
	{
		TArray<int32> Rows;
		for (const FString& Id : RowIds)
		{
			const int32 Row = Bank->GetIndexForId(FName(*Id));
			if (Row == INDEX_NONE)
			{
				return JsonError(FString::Printf(TEXT("id not in bank: %s"), *Id));
			}
			Rows.AddUnique(Row);
		}
		for (const int32 Row : RowIndices)
		{
			if (Row < 0 || Row >= Bank->Count)
			{
				return JsonError(FString::Printf(TEXT("row out of range: %d"), Row));
			}
			Rows.AddUnique(Row);
		}
		if (Rows.Num() == 0)
		{
			return JsonError(TEXT("no prototype members given"));
		}
		if (!Subsystem->MakeCentroidQuery(Bank, Rows, Query))
		{
			return JsonError(
				TEXT("centroid failed (members cancel to zero norm on a Cosine bank?)"));
		}
	}
	return RunBankQuery(Bank, Query, K, /*bScoreAsDot*/ false);
}

FString USuperFAISSToolset::ProjectAxis(const FString& BankPath,
	const TArray<float>& VectorA, const TArray<float>& VectorB, int32 K)
{
	FString Error;
	USuperFAISSVectorBank* Bank = LoadBank(BankPath, Error);
	if (Bank == nullptr)
	{
		return JsonError(Error);
	}
	if (VectorA.Num() != Bank->Dims || VectorB.Num() != Bank->Dims)
	{
		return JsonError(FString::Printf(TEXT("vectors must have %d dims"), Bank->Dims));
	}
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (Subsystem == nullptr)
	{
		return JsonError(TEXT("subsystem unavailable"));
	}
	TArray<float> Direction;
	if (!Subsystem->MakeDirectionQuery(VectorA, VectorB, Direction))
	{
		return JsonError(TEXT("no direction (A equals B?)"));
	}
	// On L2 banks the direction ranks through the dot path; identity elsewhere.
	return RunBankQuery(Bank, Direction, K,
		/*bScoreAsDot*/ Bank->Metric == ESuperFAISSBankMetric::L2);
}

namespace
{
	// Parse a metric name; empty falls back to Fallback. Returns false on an unknown name.
	bool ParseMetricName(const FString& Name, ESuperFAISSBankMetric Fallback,
		ESuperFAISSBankMetric& OutMetric)
	{
		if (Name.IsEmpty())
		{
			OutMetric = Fallback;
			return true;
		}
		if (Name.Equals(TEXT("Dot"), ESearchCase::IgnoreCase))
		{
			OutMetric = ESuperFAISSBankMetric::Dot;
		}
		else if (Name.Equals(TEXT("Cosine"), ESearchCase::IgnoreCase))
		{
			OutMetric = ESuperFAISSBankMetric::Cosine;
		}
		else if (Name.Equals(TEXT("L2"), ESearchCase::IgnoreCase))
		{
			OutMetric = ESuperFAISSBankMetric::L2;
		}
		else
		{
			return false;
		}
		return true;
	}

	// The selection, or every row 0..Count-1 when the caller left it empty.
	TArray<int32> RowsOrAll(const TArray<int32>& RowIndices, int32 Count)
	{
		if (RowIndices.Num() > 0)
		{
			return RowIndices;
		}
		TArray<int32> All;
		All.SetNumUninitialized(Count);
		for (int32 i = 0; i < Count; ++i)
		{
			All[i] = i;
		}
		return All;
	}
}

FString USuperFAISSToolset::ProjectionReport(const FString& BankPath,
	const TArray<float>& VectorA, const TArray<float>& VectorB, const TArray<int32>& GroupA)
{
	FString Error;
	USuperFAISSVectorBank* Bank = LoadBank(BankPath, Error);
	if (Bank == nullptr)
	{
		return JsonError(Error);
	}
	if (VectorA.Num() != Bank->Dims || VectorB.Num() != Bank->Dims)
	{
		return JsonError(FString::Printf(TEXT("vectors must have %d dims"), Bank->Dims));
	}
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (Subsystem == nullptr)
	{
		return JsonError(TEXT("subsystem unavailable"));
	}
	TArray<float> Projections;
	float Separation = 0.0f;
	if (!Subsystem->ProjectionReport(Bank, VectorA, VectorB, GroupA, Projections, Separation))
	{
		return JsonError(
			TEXT("projection report rejected (A equals B, empty bank, or group covers all rows?)"));
	}
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("bank"), Bank->GetPathName());
	Object->SetNumberField(TEXT("count"), Projections.Num());
	TArray<TSharedPtr<FJsonValue>> ProjValues;
	ProjValues.Reserve(Projections.Num());
	for (const float P : Projections)
	{
		ProjValues.Add(MakeShared<FJsonValueNumber>(P));
	}
	Object->SetArrayField(TEXT("projections"), ProjValues);
	if (GroupA.Num() > 0)
	{
		Object->SetNumberField(TEXT("separation"), Separation);
	}
	return ToJson(Object);
}

FString USuperFAISSToolset::SetToSetDistance(const FString& BankPathA,
	const FString& BankPathB, const TArray<int32>& RowIndicesA,
	const TArray<int32>& RowIndicesB, const FString& Metric, const FString& Mode)
{
	FString ErrorA;
	USuperFAISSVectorBank* BankA = LoadBank(BankPathA, ErrorA);
	if (BankA == nullptr)
	{
		return JsonError(ErrorA);
	}
	FString ErrorB;
	USuperFAISSVectorBank* BankB = LoadBank(BankPathB, ErrorB);
	if (BankB == nullptr)
	{
		return JsonError(ErrorB);
	}
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (Subsystem == nullptr)
	{
		return JsonError(TEXT("subsystem unavailable"));
	}
	ESuperFAISSBankMetric MetricEnum;
	if (!ParseMetricName(Metric, BankA->Metric, MetricEnum))
	{
		return JsonError(TEXT("metric must be Dot, Cosine, or L2"));
	}

	const FString ModeLower = Mode.IsEmpty() ? TEXT("all") : Mode.ToLower();
	const bool bCentroid = ModeLower == TEXT("all") || ModeLower == TEXT("centroid");
	const bool bMeanNN = ModeLower == TEXT("all") || ModeLower == TEXT("meannn");
	const bool bMaxNN = ModeLower == TEXT("all") || ModeLower == TEXT("maxnn");
	if (!bCentroid && !bMeanNN && !bMaxNN)
	{
		return JsonError(TEXT("mode must be centroid, meanNN, maxNN, or all"));
	}

	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("bankA"), BankA->GetPathName());
	Object->SetStringField(TEXT("bankB"), BankB->GetPathName());

	if (bCentroid)
	{
		const TArray<int32> IdxA = RowsOrAll(RowIndicesA, BankA->Count);
		const TArray<int32> IdxB = RowsOrAll(RowIndicesB, BankB->Count);
		float Distance = 0.0f;
		if (!Subsystem->SetToSetDistanceCrossDevice(BankA, IdxA, {}, BankB, IdxB, {},
				MetricEnum, Distance))
		{
			return JsonError(
				TEXT("centroid distance rejected (non-int8, dims mismatch, or zero-norm centroid?)"));
		}
		Object->SetNumberField(TEXT("centroidDistance"), Distance);
		Object->SetStringField(TEXT("metric"), MetricName(MetricEnum));
	}
	if (bMeanNN)
	{
		float Value = 0.0f;
		if (!Subsystem->MeanNearestNeighborCrossDevice(BankA, BankB, Value))
		{
			return JsonError(TEXT("meanNN rejected (non-int8 or dims mismatch?)"));
		}
		Object->SetNumberField(TEXT("meanNN"), Value);
	}
	if (bMaxNN)
	{
		float Value = 0.0f;
		if (!Subsystem->MaxNearestNeighborCrossDevice(BankA, BankB, Value))
		{
			return JsonError(TEXT("maxNN rejected (non-int8 or dims mismatch?)"));
		}
		Object->SetNumberField(TEXT("maxNN"), Value);
	}
	return ToJson(Object);
}

FString USuperFAISSToolset::BankSpread(const FString& BankPath,
	const TArray<int32>& RowIndices)
{
	FString Error;
	USuperFAISSVectorBank* Bank = LoadBank(BankPath, Error);
	if (Bank == nullptr)
	{
		return JsonError(Error);
	}
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (Subsystem == nullptr)
	{
		return JsonError(TEXT("subsystem unavailable"));
	}
	const TArray<int32> Idx = RowsOrAll(RowIndices, Bank->Count);
	float SpreadMean = 0.0f;
	float SpreadMax = 0.0f;
	if (!Subsystem->BankSpreadCrossDevice(Bank, Idx, ESuperFAISSReduce::Mean, SpreadMean) ||
		!Subsystem->BankSpreadCrossDevice(Bank, Idx, ESuperFAISSReduce::Max, SpreadMax))
	{
		return JsonError(TEXT("spread rejected (non-int8 bank or empty selection?)"));
	}
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("bank"), Bank->GetPathName());
	Object->SetNumberField(TEXT("count"), Idx.Num());
	Object->SetNumberField(TEXT("spreadMean"), SpreadMean);
	Object->SetNumberField(TEXT("spreadMax"), SpreadMax);
	return ToJson(Object);
}

FString USuperFAISSToolset::ImportBank(const FString& SidecarJsonPath,
	const FString& DestinationPackagePath, const FString& Quantization,
	bool bAllowOverwrite)
{
	// Destination confined to /Game; collision refused without explicit overwrite —
	// the read/import-only posture stays true rather than approximately true.
	if (!DestinationPackagePath.StartsWith(TEXT("/Game/")))
	{
		return JsonError(TEXT("destination must be under /Game"));
	}
	if (!FPackageName::IsValidLongPackageName(DestinationPackagePath))
	{
		return JsonError(FString::Printf(
			TEXT("invalid package path: %s"), *DestinationPackagePath));
	}
	if (!FPaths::FileExists(SidecarJsonPath))
	{
		return JsonError(FString::Printf(TEXT("no sidecar at %s"), *SidecarJsonPath));
	}
	ESuperFAISSBankQuantization Quant;
	if (Quantization.Equals(TEXT("Int8"), ESearchCase::IgnoreCase))
	{
		Quant = ESuperFAISSBankQuantization::Int8;
	}
	else if (Quantization.Equals(TEXT("Float32"), ESearchCase::IgnoreCase))
	{
		Quant = ESuperFAISSBankQuantization::Float32;
	}
	else
	{
		return JsonError(TEXT("quantization must be Int8 or Float32"));
	}

	const FString AssetName = FPackageName::GetLongPackageAssetName(DestinationPackagePath);
	UPackage* Package = CreatePackage(*DestinationPackagePath);
	Package->FullyLoad();
	if (UObject* Existing = StaticFindObject(
		USuperFAISSVectorBank::StaticClass(), Package, *AssetName))
	{
		if (!bAllowOverwrite)
		{
			return JsonError(FString::Printf(
				TEXT("asset exists at %s; pass bAllowOverwrite to replace"),
				*DestinationPackagePath));
		}
		Existing->Rename(
			*MakeUniqueObjectName(GetTransientPackage(), Existing->GetClass()).ToString(),
			GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
		Existing->ClearFlags(RF_Public | RF_Standalone);
	}

	FString ImportError;
	USuperFAISSVectorBank* Bank = FSuperFAISSBankImport::Import(
		SidecarJsonPath, Package, FName(*AssetName), Quant, ImportError);
	if (Bank == nullptr)
	{
		return JsonError(FString::Printf(TEXT("import rejected: %s"), *ImportError));
	}

	const FString FileName = FPackageName::LongPackageNameToFilename(
		DestinationPackagePath, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	if (!UPackage::SavePackage(Package, Bank, *FileName, SaveArgs))
	{
		return JsonError(FString::Printf(TEXT("save failed: %s"), *FileName));
	}
	FAssetRegistryModule::AssetCreated(Bank);

	const TSharedRef<FJsonObject> Object = BankSummary(*Bank);
	Object->SetNumberField(TEXT("recallAt10"), Bank->RecallAt10);
	Object->SetNumberField(TEXT("crossDeviceRecallAt10"), Bank->CrossDeviceRecallAt10);
	return ToJson(Object);
}

FString USuperFAISSToolset::ValidateBanks()
{
	FAssetRegistryModule& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	AssetRegistry.Get().SearchAllAssets(/*bSynchronousSearch*/ true);
	TArray<FAssetData> Assets;
	AssetRegistry.Get().GetAssetsByClass(
		USuperFAISSVectorBank::StaticClass()->GetClassPathName(), Assets);
	for (const FAssetData& Asset : Assets)
	{
		Asset.GetAsset();
	}
	TArray<FString> Invalid;
	const int32 InvalidCount = FSuperFAISSBankImport::ValidateLoadedBanks(Invalid);

	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("banks"), Assets.Num());
	Object->SetNumberField(TEXT("invalid"), InvalidCount);
	TArray<TSharedPtr<FJsonValue>> InvalidValues;
	for (const FString& Path : Invalid)
	{
		InvalidValues.Add(MakeShared<FJsonValueString>(Path));
	}
	Object->SetArrayField(TEXT("invalidBanks"), InvalidValues);
	return ToJson(Object);
}

FString USuperFAISSToolset::LintBank(const FString& BankPath, float DuplicateThreshold,
	int32 SampleLimit, float VarianceEpsilon)
{
	FString Error;
	const USuperFAISSVectorBank* Bank = LoadBank(BankPath, Error);
	if (Bank == nullptr)
	{
		return JsonError(Error);
	}

	FSuperFAISSLintReport Report;
	if (!FSuperFAISSBankLint::FindNearDuplicates(
			Bank, DuplicateThreshold, FMath::Max(SampleLimit, 1), Report) ||
		!FSuperFAISSBankLint::FindLowVarianceDims(Bank, VarianceEpsilon, Report))
	{
		return JsonError(TEXT("lint failed"));
	}
	// Channel banks: per-channel near-duplicates, degenerate channels, weak
	// channels (plan section 11 / T-044 W2c) - same on-demand posture.
	if (Bank->GetChannelCount() > 0)
	{
		for (const FName& Channel : Bank->ChannelNames)
		{
			FSuperFAISSBankLint::FindNearDuplicatesInChannel(
				Bank, Channel, DuplicateThreshold, FMath::Max(SampleLimit, 1), Report);
		}
		FSuperFAISSBankLint::FindDegenerateChannels(Bank, VarianceEpsilon, Report);
		FSuperFAISSBankLint::FindWeakChannels(Bank, 0.01f, Report);
	}

	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("bank"), Bank->GetPathName());
	Object->SetNumberField(TEXT("rowsExamined"), Report.RowsExamined);
	Object->SetBoolField(TEXT("sampled"), Report.bSampled);
	TArray<TSharedPtr<FJsonValue>> DupValues;
	for (const FSuperFAISSNearDuplicate& Dup : Report.NearDuplicates)
	{
		const TSharedRef<FJsonObject> DupObject = MakeShared<FJsonObject>();
		DupObject->SetNumberField(TEXT("rowA"), Dup.RowA);
		DupObject->SetNumberField(TEXT("rowB"), Dup.RowB);
		DupObject->SetNumberField(TEXT("score"), Dup.Score);
		if (!Dup.Channel.IsNone())
		{
			DupObject->SetStringField(TEXT("channel"), Dup.Channel.ToString());
		}
		DupValues.Add(MakeShared<FJsonValueObject>(DupObject));
	}
	Object->SetArrayField(TEXT("nearDuplicates"), DupValues);
	TArray<TSharedPtr<FJsonValue>> DimValues;
	for (const int32 Dim : Report.LowVarianceDims)
	{
		DimValues.Add(MakeShared<FJsonValueNumber>(Dim));
	}
	Object->SetArrayField(TEXT("lowVarianceDims"), DimValues);
	if (Bank->GetChannelCount() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> DeadValues;
		for (const FName& Dead : Report.DegenerateChannels)
		{
			DeadValues.Add(MakeShared<FJsonValueString>(Dead.ToString()));
		}
		Object->SetArrayField(TEXT("degenerateChannels"), DeadValues);
		TArray<TSharedPtr<FJsonValue>> WeakValues;
		for (const FSuperFAISSWeakChannel& Weak : Report.WeakChannels)
		{
			const TSharedRef<FJsonObject> WeakObject = MakeShared<FJsonObject>();
			WeakObject->SetStringField(TEXT("channel"), Weak.Channel.ToString());
			WeakObject->SetNumberField(TEXT("rowsBelowFloor"), Weak.RowsBelowFloor);
			WeakObject->SetNumberField(TEXT("worstEnergyFraction"),
				Weak.WorstEnergyFraction);
			WeakValues.Add(MakeShared<FJsonValueObject>(WeakObject));
		}
		Object->SetArrayField(TEXT("weakChannels"), WeakValues);
	}
	return ToJson(Object);
}

namespace
{
	USuperFAISSScratchBank* FindScratchBank(const FString& Path, FString& OutError)
	{
		for (TObjectIterator<USuperFAISSScratchBank> It; It; ++It)
		{
			if (It->GetPathName() == Path)
			{
				return *It;
			}
		}
		OutError = FString::Printf(TEXT("no live scratch bank at %s"), *Path);
		return nullptr;
	}

	TSharedRef<FJsonObject> ScratchSummary(const USuperFAISSScratchBank& Bank)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("path"), Bank.GetPathName());
		Object->SetBoolField(TEXT("initialized"), Bank.IsInitialized());
		Object->SetNumberField(TEXT("count"), Bank.GetCount());
		Object->SetNumberField(TEXT("liveCount"), Bank.GetLiveCount());
		Object->SetNumberField(TEXT("capacity"), Bank.GetCapacity());
		Object->SetNumberField(TEXT("dims"), Bank.GetDims());
		Object->SetStringField(TEXT("metric"), MetricName(Bank.GetMetric()));
		Object->SetStringField(TEXT("quantization"),
			Bank.GetQuantization() == ESuperFAISSBankQuantization::Int8 ? TEXT("Int8")
			                                                            : TEXT("Float32"));
		// V2.3 recall audit: the retention flag is a bank property, stated always;
		// the recall report appears once game code measured one, WITH its generation
		// stamp and stale mark — a stale report reads as stale here, never silently
		// current. Read-only: the report is taken by game code, never through MCP.
		Object->SetBoolField(TEXT("retainsFloats"), Bank.RetainsFloats());
		FSuperFAISSScratchRecallReport Report;
		bool bStale = false;
		if (Bank.GetLastRecallReport(Report, bStale))
		{
			const TSharedRef<FJsonObject> Recall = MakeShared<FJsonObject>();
			Recall->SetNumberField(TEXT("recall"), Report.Recall);
			Recall->SetNumberField(TEXT("k"), Report.K);
			Recall->SetNumberField(TEXT("sampleCount"), Report.SampleCount);
			Recall->SetNumberField(TEXT("liveRows"), Report.LiveRows);
			Recall->SetNumberField(TEXT("generation"),
				static_cast<double>(Report.Generation));
			Recall->SetBoolField(TEXT("informative"), Report.bInformative);
			Recall->SetBoolField(TEXT("stale"), bStale);
			Object->SetObjectField(TEXT("recallReport"), Recall);
		}
		return Object;
	}
}

FString USuperFAISSToolset::ListScratchBanks()
{
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Banks;
	for (TObjectIterator<USuperFAISSScratchBank> It; It; ++It)
	{
		Banks.Add(MakeShared<FJsonValueObject>(ScratchSummary(**It)));
	}
	Object->SetNumberField(TEXT("count"), Banks.Num());
	Object->SetArrayField(TEXT("scratchBanks"), Banks);
	return ToJson(Object);
}

FString USuperFAISSToolset::DescribeScratchBank(const FString& ScratchBankPath)
{
	FString Error;
	const USuperFAISSScratchBank* Bank = FindScratchBank(ScratchBankPath, Error);
	if (Bank == nullptr)
	{
		return JsonError(Error);
	}
	return ToJson(ScratchSummary(*Bank));
}

FString USuperFAISSToolset::QueryScratchBank(const FString& ScratchBankPath,
	const TArray<float>& Vector, int32 K)
{
	FString Error;
	USuperFAISSScratchBank* Bank = FindScratchBank(ScratchBankPath, Error);
	if (Bank == nullptr)
	{
		return JsonError(Error);
	}
	if (!Bank->IsInitialized())
	{
		return JsonError(TEXT("scratch bank is not initialized"));
	}
	if (Vector.Num() != Bank->GetDims())
	{
		return JsonError(FString::Printf(TEXT("vector has %d dims; bank has %d"),
			Vector.Num(), Bank->GetDims()));
	}
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (Subsystem == nullptr)
	{
		return JsonError(TEXT("subsystem unavailable"));
	}
	FSuperFAISSQueryArgs Args;
	Args.K = FMath::Clamp(K, 1, 1000);
	TArray<FSuperFAISSHit> Hits;
	if (!Subsystem->QueryScratch(Bank, Vector, Args, Hits))
	{
		return JsonError(TEXT("query rejected (bank draining or invalid vector)"));
	}
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("scratchBank"), Bank->GetPathName());
	TArray<TSharedPtr<FJsonValue>> HitValues;
	for (const FSuperFAISSHit& Hit : Hits)
	{
		const TSharedRef<FJsonObject> HitObject = MakeShared<FJsonObject>();
		HitObject->SetNumberField(TEXT("index"), Hit.Index);
		HitObject->SetNumberField(TEXT("score"), Hit.Score);
		HitObject->SetNumberField(TEXT("margin"), Hit.Margin);
		HitValues.Add(MakeShared<FJsonValueObject>(HitObject));
	}
	Object->SetArrayField(TEXT("hits"), HitValues);
	return ToJson(Object);
}

FString USuperFAISSToolset::BankSpreadScratch(const FString& ScratchBankPath)
{
	FString Error;
	USuperFAISSScratchBank* Bank = FindScratchBank(ScratchBankPath, Error);
	if (Bank == nullptr)
	{
		return JsonError(Error);
	}
	if (!Bank->IsInitialized())
	{
		return JsonError(TEXT("scratch bank is not initialized"));
	}
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (Subsystem == nullptr)
	{
		return JsonError(TEXT("subsystem unavailable"));
	}
	float SpreadMean = 0.0f;
	float SpreadMax = 0.0f;
	if (!Subsystem->BankSpreadCrossDeviceScratch(Bank, ESuperFAISSReduce::Mean, SpreadMean) ||
		!Subsystem->BankSpreadCrossDeviceScratch(Bank, ESuperFAISSReduce::Max, SpreadMax))
	{
		return JsonError(
			TEXT("spread rejected (non-int8 bank, empty snapshot, or bank draining?)"));
	}
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("scratchBank"), Bank->GetPathName());
	Object->SetNumberField(TEXT("liveCount"), Bank->GetLiveCount());
	Object->SetNumberField(TEXT("spreadMean"), SpreadMean);
	Object->SetNumberField(TEXT("spreadMax"), SpreadMax);
	return ToJson(Object);
}

FString USuperFAISSToolset::SetToSetDistanceScratch(const FString& ScratchBankPathA,
	const FString& BankPathB, const TArray<int32>& RowIndicesB, const FString& Metric,
	const FString& Mode)
{
	FString ErrorA;
	USuperFAISSScratchBank* BankA = FindScratchBank(ScratchBankPathA, ErrorA);
	if (BankA == nullptr)
	{
		return JsonError(ErrorA);
	}
	if (!BankA->IsInitialized())
	{
		return JsonError(TEXT("scratch bank is not initialized"));
	}
	FString ErrorB;
	USuperFAISSVectorBank* BankB = LoadBank(BankPathB, ErrorB);
	if (BankB == nullptr)
	{
		return JsonError(ErrorB);
	}
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (Subsystem == nullptr)
	{
		return JsonError(TEXT("subsystem unavailable"));
	}
	ESuperFAISSBankMetric MetricEnum;
	if (!ParseMetricName(Metric, BankA->GetMetric(), MetricEnum))
	{
		return JsonError(TEXT("metric must be Dot, Cosine, or L2"));
	}

	const FString ModeLower = Mode.IsEmpty() ? TEXT("all") : Mode.ToLower();
	const bool bCentroid = ModeLower == TEXT("all") || ModeLower == TEXT("centroid");
	const bool bMeanNN = ModeLower == TEXT("all") || ModeLower == TEXT("meannn");
	const bool bMaxNN = ModeLower == TEXT("all") || ModeLower == TEXT("maxnn");
	if (!bCentroid && !bMeanNN && !bMaxNN)
	{
		return JsonError(TEXT("mode must be centroid, meanNN, maxNN, or all"));
	}

	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("scratchBankA"), BankA->GetPathName());
	Object->SetStringField(TEXT("bankB"), BankB->GetPathName());

	if (bCentroid)
	{
		const TArray<int32> IdxB = RowsOrAll(RowIndicesB, BankB->Count);
		float Distance = 0.0f;
		if (!Subsystem->SetToSetDistanceCrossDeviceScratch(BankA, BankB, IdxB, {},
				MetricEnum, Distance))
		{
			return JsonError(
				TEXT("centroid distance rejected (non-int8, dims mismatch, empty snapshot, or zero-norm centroid?)"));
		}
		Object->SetNumberField(TEXT("centroidDistance"), Distance);
		Object->SetStringField(TEXT("metric"), MetricName(MetricEnum));
	}
	if (bMeanNN)
	{
		float Value = 0.0f;
		if (!Subsystem->MeanNearestNeighborCrossDeviceScratch(BankA, BankB, Value))
		{
			return JsonError(TEXT("meanNN rejected (non-int8, dims mismatch, or empty snapshot?)"));
		}
		Object->SetNumberField(TEXT("meanNN"), Value);
	}
	if (bMaxNN)
	{
		float Value = 0.0f;
		if (!Subsystem->MaxNearestNeighborCrossDeviceScratch(BankA, BankB, Value))
		{
			return JsonError(TEXT("maxNN rejected (non-int8, dims mismatch, or empty snapshot?)"));
		}
		Object->SetNumberField(TEXT("maxNN"), Value);
	}
	return ToJson(Object);
}
