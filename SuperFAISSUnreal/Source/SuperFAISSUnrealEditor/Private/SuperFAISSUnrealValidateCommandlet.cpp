#include "SuperFAISSUnrealValidateCommandlet.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "SuperFAISSBankImport.h"
#include "SuperFAISSBankLint.h"
#include "SuperFAISSVectorBank.h"

int32 USuperFAISSUnrealValidateCommandlet::Main(const FString& Params)
{
	FAssetRegistryModule& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	AssetRegistry.Get().SearchAllAssets(/*bSynchronousSearch*/ true);

	TArray<FAssetData> Assets;
	AssetRegistry.Get().GetAssetsByClass(
		USuperFAISSVectorBank::StaticClass()->GetClassPathName(), Assets);

	// Loading runs PostLoad, which validates; ValidateLoadedBanks then sweeps.
	for (const FAssetData& Asset : Assets)
	{
		Asset.GetAsset();
	}

	TArray<FString> Invalid;
	const int32 InvalidCount = FSuperFAISSBankImport::ValidateLoadedBanks(Invalid);
	for (const FString& Path : Invalid)
	{
		UE_LOG(LogTemp, Error, TEXT("invalid bank: %s"), *Path);
	}
	UE_LOG(LogTemp, Display, TEXT("SuperFAISSUnrealValidate: %d banks, %d invalid"),
		Assets.Num(), InvalidCount);

	// Optional health analyses (plan 18.2), on demand only: -Lint enables them;
	// thresholds and the near-duplicate sample cap are overridable. Lint findings
	// are warnings, not validation failures — the exit code stays validity-only.
	if (FParse::Param(*Params, TEXT("Lint")))
	{
		float DupThreshold = 0.999f;
		FParse::Value(*Params, TEXT("DupThreshold="), DupThreshold);
		float VarianceEpsilon = 1e-8f;
		FParse::Value(*Params, TEXT("VarianceEpsilon="), VarianceEpsilon);
		int32 SampleLimit = 4096;
		FParse::Value(*Params, TEXT("LintSampleLimit="), SampleLimit);

		for (const FAssetData& Asset : Assets)
		{
			const USuperFAISSVectorBank* Bank =
				Cast<USuperFAISSVectorBank>(Asset.GetAsset());
			if (Bank == nullptr || !Bank->IsValid())
			{
				continue;
			}
			FSuperFAISSLintReport Report;
			FSuperFAISSBankLint::FindNearDuplicates(Bank, DupThreshold, SampleLimit, Report);
			FSuperFAISSBankLint::FindLowVarianceDims(Bank, VarianceEpsilon, Report);
			for (const FSuperFAISSNearDuplicate& Dup : Report.NearDuplicates)
			{
				UE_LOG(LogTemp, Warning,
					TEXT("%s: near-duplicate rows %d and %d (runner-up score %g)"),
					*Asset.AssetName.ToString(), Dup.RowA, Dup.RowB, Dup.Score);
			}
			for (const int32 Dim : Report.LowVarianceDims)
			{
				UE_LOG(LogTemp, Warning, TEXT("%s: low-variance dim %d"),
					*Asset.AssetName.ToString(), Dim);
			}
			UE_LOG(LogTemp, Display,
				TEXT("%s: lint examined %d rows%s — %d near-duplicate pair(s), %d low-variance dim(s)"),
				*Asset.AssetName.ToString(), Report.RowsExamined,
				Report.bSampled ? TEXT(" (sampled)") : TEXT(""),
				Report.NearDuplicates.Num(), Report.LowVarianceDims.Num());
		}
	}
	return InvalidCount == 0 ? 0 : 1;
}
