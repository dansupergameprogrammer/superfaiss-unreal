#pragma once

// S-INSP-3.3.1 Gate 1a: the independent oracle's bake step. Loads the hand-authored
// coordinate sidecar (TutorialBankGeometry.csv, this directory) and turns each row's
// symbolic (ChanADir, ChanBDir) tag pair into the real float vector the sidecar's own
// header comment defines, then bakes that geometry into either an asset
// (USuperFAISSVectorBank, via InitFromSource) or a scratch archive
// (USuperFAISSScratchBank, via Init+Append(+Remove)+SaveToBytes) -- the two
// FSuperFAISSInspectionSource kinds the oracle-gated tickets (SF34-003/004/005/006) must
// answer identically over.
//
// This header is NOT production code: it lives under Private/Tests/, is compiled only
// inside WITH_DEV_AUTOMATION_TESTS translation units, and is read by no runtime or editor
// module path. The sidecar is plain, human-checkable text -- the "bake" is a parse plus
// the closed-form direction-to-vector rule the CSV's own header states, not a new binary
// persistence format (dim 9's Watch: no versioned field is introduced here; see the
// test-design artifact for the explicit statement).

#include "CoreMinimal.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SuperFAISSScratchBank.h"
#include "SuperFAISSVectorBank.h"

namespace SuperFAISSTutorialBank
{
	// Geometry constants (mirrors the sidecar's own header comment -- kept here so a
	// reader of the test code sees the same rule without cross-referencing the CSV).
	constexpr int32 kChannelDims = 4;
	constexpr int32 kDims = 2 * kChannelDims; // chanA [0,4) + chanB [4,8)
	constexpr float kMagnitude = 10.0f;

	struct FTutorialRow
	{
		FString Bank; // "Primary" or "Secondary"
		int32 Index = INDEX_NONE;
		int32 ChanADir = 0;
		int32 ChanBDir = 0;
		FString Role;
	};

	// Parses the sidecar. Returns false (and logs a test error) if the file cannot be
	// found or a row is malformed -- a fixture failure is reported the same way any other
	// setup failure is in this suite's existing helpers (MakeBank et al.), never silently
	// producing an empty/degenerate bank that would make every downstream oracle
	// assertion fail for the wrong reason.
	inline bool LoadSidecar(FAutomationTestBase& Test, TArray<FTutorialRow>& OutRows)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SuperFAISSUnreal"));
		if (!Plugin.IsValid())
		{
			Test.AddError(TEXT("tutorial bank fixture: SuperFAISSUnreal plugin not found"));
			return false;
		}
		const FString SidecarPath = FPaths::Combine(Plugin->GetBaseDir(),
			TEXT("Source/SuperFAISSUnrealEditor/Private/Tests/Fixtures/TutorialBankGeometry.csv"));
		TArray<FString> Lines;
		if (!FFileHelper::LoadFileToStringArray(Lines, *SidecarPath))
		{
			Test.AddError(FString::Printf(TEXT("tutorial bank fixture: cannot read sidecar at %s"), *SidecarPath));
			return false;
		}
		for (const FString& Line : Lines)
		{
			const FString Trimmed = Line.TrimStartAndEnd();
			if (Trimmed.IsEmpty() || Trimmed.StartsWith(TEXT("#")) || Trimmed.StartsWith(TEXT("Bank,")))
			{
				continue; // comment / header line
			}
			TArray<FString> Fields;
			Trimmed.ParseIntoArray(Fields, TEXT(","), true);
			if (Fields.Num() != 5)
			{
				Test.AddError(FString::Printf(TEXT("tutorial bank fixture: malformed sidecar row: %s"), *Trimmed));
				return false;
			}
			FTutorialRow Row;
			Row.Bank = Fields[0];
			Row.Index = FCString::Atoi(*Fields[1]);
			Row.ChanADir = FCString::Atoi(*Fields[2]);
			Row.ChanBDir = FCString::Atoi(*Fields[3]);
			Row.Role = Fields[4];
			OutRows.Add(MoveTemp(Row));
		}
		return true;
	}

	// The closed-form direction-to-vector rule (the sidecar header's own definition):
	// 10.0 on basis element ChanADir within chanA, 10.0 on basis element ChanBDir within
	// chanB, zero elsewhere.
	inline void AppendRowVector(TArray<float>& Rows, int32 ChanADir, int32 ChanBDir)
	{
		const int32 Base = Rows.Num();
		Rows.AddZeroed(kDims);
		Rows[Base + ChanADir] = kMagnitude;
		Rows[Base + kChannelDims + ChanBDir] = kMagnitude;
	}

	// Loads and bakes one named bank ("Primary" or "Secondary") from the sidecar into a
	// flat row-major float array, ordered by ascending Index (the sidecar is already
	// written in that order; this re-sorts defensively so a future edit to the file's row
	// order can never silently transpose two rows). Returns the row count.
	inline bool BakeRows(FAutomationTestBase& Test, const FString& BankName, TArray<float>& OutRows,
		int32& OutCount)
	{
		TArray<FTutorialRow> AllRows;
		if (!LoadSidecar(Test, AllRows))
		{
			return false;
		}
		TArray<FTutorialRow> Filtered;
		for (const FTutorialRow& Row : AllRows)
		{
			if (Row.Bank == BankName)
			{
				Filtered.Add(Row);
			}
		}
		if (Filtered.Num() == 0)
		{
			Test.AddError(FString::Printf(TEXT("tutorial bank fixture: no rows for bank '%s'"), *BankName));
			return false;
		}
		Filtered.Sort([](const FTutorialRow& A, const FTutorialRow& B) { return A.Index < B.Index; });
		OutRows.Reset();
		OutRows.Reserve(Filtered.Num() * kDims);
		for (int32 i = 0; i < Filtered.Num(); ++i)
		{
			if (Filtered[i].Index != i)
			{
				Test.AddError(FString::Printf(
					TEXT("tutorial bank fixture: bank '%s' index gap or duplicate at position %d (found Index=%d)"),
					*BankName, i, Filtered[i].Index));
				return false;
			}
			AppendRowVector(OutRows, Filtered[i].ChanADir, Filtered[i].ChanBDir);
		}
		OutCount = Filtered.Num();
		return true;
	}

	// Bakes the named bank as an in-memory ASSET (USuperFAISSVectorBank), channel table
	// {chanA=[0,4), chanB=[4,8)}, metric=Cosine, quant=Float32 (exact -- see the sidecar
	// header). Fails the test and returns nullptr on any bake step failure.
	inline USuperFAISSVectorBank* BakeAsAsset(FAutomationTestBase& Test, const FString& BankName)
	{
		TArray<float> Rows;
		int32 Count = 0;
		if (!BakeRows(Test, BankName, Rows, Count))
		{
			return nullptr;
		}
		const TArray<FName> ChannelNames = {TEXT("chanA"), TEXT("chanB")};
		const TArray<int32> ChannelOffsets = {0, kChannelDims};
		const TArray<int32> ChannelLengths = {kChannelDims, kChannelDims};
		USuperFAISSVectorBank* Bank = NewObject<USuperFAISSVectorBank>();
		FString Error;
		const bool bOk = Bank->InitFromSource(Rows, Count, kDims, ESuperFAISSBankMetric::Cosine,
			ESuperFAISSBankQuantization::Float32, {}, TEXT("tutorial-bank-asset"), Error,
			ChannelNames, ChannelOffsets, ChannelLengths);
		Test.TestTrue(FString::Printf(TEXT("tutorial bank '%s' asset baked: %s"), *BankName, *Error), bOk);
		return bOk ? Bank : nullptr;
	}

	// Bakes the named bank as a scratch ARCHIVE's saved bytes, with RowsToTombstone
	// Remove()'d before Save -- the pruned-archive fixture SF34-004's oracle needs (a
	// tombstone in the MIDDLE of the bank, so the rows after it are the ones whose native
	// source index must survive display/reporting unshifted). Same channel geometry and
	// metric/quant as BakeAsAsset -- the whole point is that asset and archive carry
	// IDENTICAL geometry, so any answer divergence between the two sources is the bug the
	// oracle-gated tickets exist to catch, not a fixture artifact.
	inline bool BakeAsArchiveBytes(FAutomationTestBase& Test, const FString& BankName,
		const TArray<int32>& RowsToTombstone, TArray<uint8>& OutBytes)
	{
		TArray<float> Rows;
		int32 Count = 0;
		if (!BakeRows(Test, BankName, Rows, Count))
		{
			return false;
		}
		const TArray<FName> ChannelNames = {TEXT("chanA"), TEXT("chanB")};
		const TArray<int32> ChannelOffsets = {0, kChannelDims};
		const TArray<int32> ChannelLengths = {kChannelDims, kChannelDims};

		USuperFAISSScratchBank* Scratch = NewObject<USuperFAISSScratchBank>();
		if (!Scratch->InitWithChannels(Count, kDims, ESuperFAISSBankMetric::Cosine,
				ESuperFAISSBankQuantization::Float32, ChannelNames, ChannelOffsets, ChannelLengths))
		{
			Test.AddError(FString::Printf(TEXT("tutorial bank '%s' archive: InitWithChannels failed"), *BankName));
			return false;
		}
		for (int32 i = 0; i < Count; ++i)
		{
			TArray<float> Row;
			Row.Append(&Rows[static_cast<int64>(i) * kDims], kDims);
			int32 OutIndex = INDEX_NONE;
			if (!Scratch->Append(Row, OutIndex))
			{
				Test.AddError(FString::Printf(TEXT("tutorial bank '%s' archive: Append row %d failed"), *BankName, i));
				return false;
			}
		}
		for (const int32 Idx : RowsToTombstone)
		{
			if (!Scratch->Remove(Idx))
			{
				Test.AddError(FString::Printf(TEXT("tutorial bank '%s' archive: Remove row %d failed"), *BankName, Idx));
				return false;
			}
		}
		if (!Scratch->SaveToBytes(OutBytes))
		{
			Test.AddError(FString::Printf(TEXT("tutorial bank '%s' archive: SaveToBytes failed"), *BankName));
			return false;
		}
		return true;
	}
}
