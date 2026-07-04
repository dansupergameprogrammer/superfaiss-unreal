#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class USuperFAISSVectorBank;

// The 18.2 inspection surface, one dockable editor tab with two panes:
// - Live query inspector: pick a bank, type a row id (or index), see ranked matches
//   with margins — the demo's typed-word mode as a reusable editor tool. No encoder:
//   text works only on banks whose ids are the vocabulary (the 18.4 boundary rule).
//   On channel banks (schema 2) a weight slider per named channel drives the query,
//   and every hit carries decomposition bars — per-channel contributions from
//   DecomposeHit, which sum exactly to the score (V2 plan section 6). Displayed
//   per-channel cosines clamp to [-1, 1] (T-044 W2d: int8 quantization noise can
//   push a shade past 1; the clamp is display-only and marked when it fires).
// - Projection visualizer: PCA point cloud of the bank (2 components), computed on
//   demand over a deterministic stride sample (N1: bounded, never silently
//   exhaustive on a large bank). On channel banks the projection can be scoped to
//   one named channel's sub-range — that channel's own cluster structure.
class SSuperFAISSBankInspector : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSuperFAISSBankInspector) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	void RefreshBankList();
	USuperFAISSVectorBank* GetSelectedBank() const;
	void OnBankSelected();
	void RunQuery(const FString& Text);
	void ComputeProjection();
	FString BankInfoLine() const;

	TArray<TSharedPtr<FString>> BankNames;
	TArray<TWeakObjectPtr<USuperFAISSVectorBank>> BankAssets;
	TSharedPtr<FString> SelectedBankName;

	TArray<TSharedPtr<FString>> ResultLines;
	TSharedPtr<class SListView<TSharedPtr<FString>>> ResultList;

	// Channel query state: one weight per channel of the selected bank, slider-driven.
	TArray<TSharedPtr<FString>> ChannelSliderNames; // parallel to ChannelWeights
	TArray<float> ChannelWeights;
	TSharedPtr<class SVerticalBox> ChannelSliderBox;
	void RebuildChannelSliders();

	// Projection state: sampled 2D coords, normalized into [0,1] for the paint pass.
	TArray<FVector2f> ProjectedPoints;
	FString ProjectionStatus;

	// Projection scope: "(whole row)" + the selected bank's channel names.
	TArray<TSharedPtr<FString>> ProjectionScopes;
	TSharedPtr<FString> SelectedProjectionScope;

	static constexpr int32 ProjectionSampleLimit = 2048;
	static constexpr int32 PcaIterations = 24;
};
