#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class USuperFAISSVectorBank;

// The 18.2 inspection surface, one dockable editor tab with two panes:
// - Live query inspector: pick a bank, type a row id (or index), see ranked matches
//   with margins — the demo's typed-word mode as a reusable editor tool. No encoder:
//   text works only on banks whose ids are the vocabulary (the 18.4 boundary rule).
// - Projection visualizer: PCA point cloud of the bank (2 components), computed on
//   demand over a deterministic stride sample (N1: bounded, never silently
//   exhaustive on a large bank).
class SSuperFAISSBankInspector : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSuperFAISSBankInspector) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	void RefreshBankList();
	USuperFAISSVectorBank* GetSelectedBank() const;
	void RunQuery(const FString& Text);
	void ComputeProjection();

	TArray<TSharedPtr<FString>> BankNames;
	TArray<TWeakObjectPtr<USuperFAISSVectorBank>> BankAssets;
	TSharedPtr<FString> SelectedBankName;

	TArray<TSharedPtr<FString>> ResultLines;
	TSharedPtr<class SListView<TSharedPtr<FString>>> ResultList;

	// Projection state: sampled 2D coords, normalized into [0,1] for the paint pass.
	TArray<FVector2f> ProjectedPoints;
	FString ProjectionStatus;

	static constexpr int32 ProjectionSampleLimit = 2048;
	static constexpr int32 PcaIterations = 24;
};
