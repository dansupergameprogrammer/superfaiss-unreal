#include "SSuperFAISSBankInspector.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "SuperFAISSInspectorSettings.h"
#include "SuperFAISSInspectorSlowTask.h"
#include "SuperFAISSScratchBank.h"
#include "SuperFAISSSubsystem.h"
#include "SuperFAISSVectorBank.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STreeView.h"
#include "Widgets/Text/STextBlock.h"

#include "superfaiss/superfaiss.h"

namespace
{
	// Finding 6 (regression on Finding 1, plugin editor review): composes two native-
	// index-space exclusion word arrays (tombstone words and the zero-energy-slice words
	// BuildAnalysisSample's full-view-identity mode now emits) into one. `Other` may be
	// empty (an asset source's tombstone words, or a whole-row-scope pass with no
	// zero-energy exclusion) -- a no-op OR, matching every existing empty-array
	// convention this file already relies on. `Base` grows to cover `Other` if `Other` is
	// longer (never the reverse -- neither array is ever truncated).
	void MergeExcludeBits(TArray<uint32>& Base, const TArray<uint32>& Other)
	{
		if (Other.Num() == 0)
		{
			return;
		}
		if (Base.Num() < Other.Num())
		{
			Base.SetNumZeroed(Other.Num());
		}
		for (int32 i = 0; i < Other.Num(); ++i)
		{
			Base[i] |= Other[i];
		}
	}

	// Finding 6: the exact population a caller-composed "matchable" count needs -- the
	// number of set bits in a native-index-space exclusion array (tombstoned OR
	// zero-energy, already merged by MergeExcludeBits above). Manual Kernighan popcount,
	// no platform dependency.
	int32 CountExcludedBits(const TArray<uint32>& Bits)
	{
		int32 Count = 0;
		for (uint32 Word : Bits)
		{
			while (Word != 0)
			{
				Word &= (Word - 1);
				++Count;
			}
		}
		return Count;
	}

	// Section 25.5 View A: "cluster coloring paints the existing scatter points from a
	// fixed deterministic palette keyed by component id... the list is the primary
	// rendering, color the enhancement" (brief §7 accessibility). Twelve visually distinct
	// hues, indexed by ComponentId modulo the palette size — deterministic, no allocation.
	FLinearColor StructureClusterColor(int32 ComponentId)
	{
		static const FLinearColor Palette[] = {
			FLinearColor(0.90f, 0.30f, 0.30f), FLinearColor(0.30f, 0.75f, 0.35f),
			FLinearColor(0.30f, 0.55f, 0.95f), FLinearColor(0.95f, 0.75f, 0.20f),
			FLinearColor(0.75f, 0.35f, 0.90f), FLinearColor(0.25f, 0.80f, 0.80f),
			FLinearColor(0.95f, 0.50f, 0.20f), FLinearColor(0.55f, 0.85f, 0.30f),
			FLinearColor(0.90f, 0.35f, 0.65f), FLinearColor(0.45f, 0.45f, 0.90f),
			FLinearColor(0.65f, 0.65f, 0.25f), FLinearColor(0.35f, 0.85f, 0.60f),
		};
		const int32 N = UE_ARRAY_COUNT(Palette);
		return Palette[((ComponentId % N) + N) % N];
	}

	// The painted position of point P for a given widget Size and dot PointSize — shared by
	// OnPaint (drawing) and OnMouseMove (hit-testing) so the two never drift apart.
	FVector2D ScatterPointPaintPos(const FVector2f& P, const FVector2D& Size, double PointSize)
	{
		return FVector2D(P.X * (Size.X - PointSize - 1.0), (1.0 - P.Y) * (Size.Y - PointSize - 1.0));
	}

	// Aliased so SLATE_ARGUMENT's macro expansion never sees the raw template comma.
	using FStructureHighlightMap = TMap<int32, ESuperFAISSStructureHighlight>;

	// Scatter pane: paints the projected points; leaf widget, editor-tier. Optionally
	// colors by Structure component id (section 25.5's "1:1 scatter join") and enlarges
	// points in the selected cluster (the "selecting a cluster highlights its points"
	// clause). Hovering a point shows its label via the caller-supplied GetPointLabel
	// (a dumb painter: this widget knows sample-position geometry, never bank ids/clusters).
	class SSuperFAISSScatter : public SLeafWidget
	{
	public:
		SLATE_BEGIN_ARGS(SSuperFAISSScatter) {}
			SLATE_ARGUMENT(const TArray<FVector2f>*, Points)
			SLATE_ARGUMENT(const TArray<int32>*, ComponentIds)
			SLATE_ARGUMENT(const FStructureHighlightMap*, HighlightedSampleIndices)
			SLATE_ARGUMENT(TFunction<FText(int32)>, GetPointLabel)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			Points = InArgs._Points;
			ComponentIds = InArgs._ComponentIds;
			HighlightedSampleIndices = InArgs._HighlightedSampleIndices;
			GetPointLabel = InArgs._GetPointLabel;
			SetToolTipText(TAttribute<FText>::CreateSP(this, &SSuperFAISSScatter::GetHoverTooltipText));
		}

		virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& Geometry,
			const FSlateRect& CullingRect, FSlateWindowElementList& OutDrawElements,
			int32 LayerId, const FWidgetStyle& InWidgetStyle,
			bool bParentEnabled) const override
		{
			const FVector2D Size = Geometry.GetLocalSize();
			if (Points != nullptr)
			{
				const FSlateBrush* Brush = FCoreStyle::Get().GetBrush("GenericWhiteBox");
				const bool bHasComponentIds = ComponentIds != nullptr && ComponentIds->Num() == Points->Num();
				for (int32 i = 0; i < Points->Num(); ++i)
				{
					const FVector2f& P = (*Points)[i];
					const ESuperFAISSStructureHighlight* HighlightKind = HighlightedSampleIndices != nullptr
						? HighlightedSampleIndices->Find(i) : nullptr;
					FLinearColor Color(0.35f, 0.75f, 1.0f, 0.85f);
					if (bHasComponentIds)
					{
						const int32 ComponentId = (*ComponentIds)[i];
						// -1 (outlier / not-yet-computed) stays the plain default blue;
						// only real components get palette colors.
						if (ComponentId >= 0)
						{
							Color = StructureClusterColor(ComponentId);
						}
					}
					// Highlight kind (§25.5 follow-up: Dan couldn't tell k-nearest apart from
					// "everything in the group" once both rendered identically) — Component
					// keeps the cluster's own palette color and a modest enlarge; Neighbor and
					// Selected are the two things Dan actually clicked toward, so they get
					// distinct colors that read at a glance regardless of cluster size.
					double PointSize = 3.0;
					if (HighlightKind != nullptr)
					{
						switch (*HighlightKind)
						{
						case ESuperFAISSStructureHighlight::Selected:
							PointSize = 8.0;
							Color = FLinearColor(1.0f, 1.0f, 1.0f, 1.0f);
							break;
						case ESuperFAISSStructureHighlight::Neighbor:
							PointSize = 6.0;
							Color = FLinearColor(1.0f, 0.85f, 0.1f, 1.0f);
							break;
						case ESuperFAISSStructureHighlight::Component:
						default:
							PointSize = 5.0;
							break;
						}
					}
					// The hovered point paints slightly brighter, on top of the highlight kind.
					const bool bHovered = i == HoveredIndex;
					const FVector2D Pos = ScatterPointPaintPos(P, Size, PointSize);
					FSlateDrawElement::MakeBox(OutDrawElements, LayerId,
						Geometry.ToPaintGeometry(FVector2D(PointSize, PointSize),
							FSlateLayoutTransform(Pos)),
						Brush, ESlateDrawEffect::None, bHovered ? Color * 1.5f : Color);
				}
			}
			return LayerId + 1;
		}

		virtual FVector2D ComputeDesiredSize(float) const override
		{
			return FVector2D(420.0, 420.0);
		}

		virtual FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
		{
			const int32 Previous = HoveredIndex;
			HoveredIndex = INDEX_NONE;
			if (Points != nullptr)
			{
				const FVector2D Local = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
				const FVector2D Size = MyGeometry.GetLocalSize();
				constexpr double HitRadius = 6.0;
				double BestDistSq = HitRadius * HitRadius;
				for (int32 i = 0; i < Points->Num(); ++i)
				{
					const FVector2D Pos = ScatterPointPaintPos((*Points)[i], Size, 3.0) + FVector2D(1.5, 1.5);
					const double DistSq = FVector2D::DistSquared(Local, Pos);
					if (DistSq <= BestDistSq)
					{
						BestDistSq = DistSq;
						HoveredIndex = i;
					}
				}
			}
			if (HoveredIndex != Previous)
			{
				Invalidate(EInvalidateWidgetReason::Paint);
			}
			return FReply::Unhandled();
		}

		virtual void OnMouseLeave(const FPointerEvent& MouseEvent) override
		{
			if (HoveredIndex != INDEX_NONE)
			{
				HoveredIndex = INDEX_NONE;
				Invalidate(EInvalidateWidgetReason::Paint);
			}
			SLeafWidget::OnMouseLeave(MouseEvent);
		}

	private:
		FText GetHoverTooltipText() const
		{
			return (HoveredIndex != INDEX_NONE && GetPointLabel)
				? GetPointLabel(HoveredIndex) : FText::GetEmpty();
		}

		const TArray<FVector2f>* Points = nullptr;
		const TArray<int32>* ComponentIds = nullptr;
		const FStructureHighlightMap* HighlightedSampleIndices = nullptr;
		TFunction<FText(int32)> GetPointLabel;
		int32 HoveredIndex = INDEX_NONE;
	};

	// A compact text bar for a per-channel contribution: proportional block run,
	// negative contributions marked with '-'. Editor-list idiom, no custom paint.
	FString ContributionBar(float Value, float MaxAbs)
	{
		const int32 MaxBlocks = 10;
		const float Denominator = FMath::Max(MaxAbs, KINDA_SMALL_NUMBER);
		const int32 Blocks = FMath::Clamp(
			FMath::RoundToInt(FMath::Abs(Value) / Denominator * MaxBlocks), 0, MaxBlocks);
		FString Bar;
		for (int32 i = 0; i < Blocks; ++i)
		{
			Bar += TEXT("#");
		}
		if (Value < 0.0f && Blocks > 0)
		{
			Bar = TEXT("-") + Bar;
		}
		return Bar;
	}
}

// ---------------------------------------------------------------------------
// FSuperFAISSInspectionSource: mechanical dispatch over the two source kinds -- no
// algorithm to gate, the SAME "real, shipped-shape logic" posture already applied to
// CheckSecondBankCompatible/RefreshSecondBankList.
// ---------------------------------------------------------------------------

bool FSuperFAISSInspectionSource::IsValid() const
{
	switch (Kind)
	{
	case EKind::Asset:
		return Asset.IsValid() && Asset->IsValid();
	case EKind::Archive:
		return ArchiveBank.IsValid() && ArchiveBank->IsInitialized();
	default:
		return false;
	}
}

FString FSuperFAISSInspectionSource::DisplayName() const
{
	switch (Kind)
	{
	case EKind::Asset:
		return Asset.IsValid() ? Asset->GetName() : FString();
	case EKind::Archive:
		return ArchiveDisplayName;
	default:
		return FString();
	}
}

int32 FSuperFAISSInspectionSource::GetCount() const
{
	switch (Kind)
	{
	case EKind::Asset:
		return Asset.IsValid() ? Asset->Count : 0;
	case EKind::Archive:
		return ArchiveBank.IsValid() ? ArchiveBank->GetCount() : 0;
	default:
		return 0;
	}
}

int32 FSuperFAISSInspectionSource::GetLiveCount() const
{
	// The documented asymmetry (section 25.3): an asset carries no tombstones, so its
	// live count is its published count; an archive's is ScratchBank's own LiveCount().
	switch (Kind)
	{
	case EKind::Asset:
		return GetCount();
	case EKind::Archive:
		return ArchiveBank.IsValid() ? ArchiveBank->GetLiveCount() : 0;
	default:
		return 0;
	}
}

int32 FSuperFAISSInspectionSource::GetDims() const
{
	switch (Kind)
	{
	case EKind::Asset:
		return Asset.IsValid() ? Asset->Dims : 0;
	case EKind::Archive:
		return ArchiveBank.IsValid() ? ArchiveBank->GetDims() : 0;
	default:
		return 0;
	}
}

ESuperFAISSBankMetric FSuperFAISSInspectionSource::GetMetric() const
{
	switch (Kind)
	{
	case EKind::Asset:
		return Asset.IsValid() ? Asset->Metric : ESuperFAISSBankMetric::Dot;
	case EKind::Archive:
		return ArchiveBank.IsValid() ? ArchiveBank->GetMetric() : ESuperFAISSBankMetric::Dot;
	default:
		return ESuperFAISSBankMetric::Dot;
	}
}

ESuperFAISSBankQuantization FSuperFAISSInspectionSource::GetQuantization() const
{
	switch (Kind)
	{
	case EKind::Asset:
		return Asset.IsValid() ? Asset->Quantization : ESuperFAISSBankQuantization::Float32;
	case EKind::Archive:
		return ArchiveBank.IsValid() ? ArchiveBank->GetQuantization() : ESuperFAISSBankQuantization::Float32;
	default:
		return ESuperFAISSBankQuantization::Float32;
	}
}

int32 FSuperFAISSInspectionSource::GetChannelCount() const
{
	switch (Kind)
	{
	case EKind::Asset:
		return Asset.IsValid() ? Asset->GetChannelCount() : 0;
	case EKind::Archive:
		return ArchiveBank.IsValid() ? ArchiveBank->GetChannelCount() : 0;
	default:
		return 0;
	}
}

int32 FSuperFAISSInspectionSource::GetChannelIndex(FName Name) const
{
	switch (Kind)
	{
	case EKind::Asset:
		return Asset.IsValid() ? Asset->GetChannelIndex(Name) : INDEX_NONE;
	case EKind::Archive:
		return ArchiveBank.IsValid() ? ArchiveBank->GetChannelIndex(Name) : INDEX_NONE;
	default:
		return INDEX_NONE;
	}
}

FName FSuperFAISSInspectionSource::GetIdForIndex(int32 Index) const
{
	// An archive carries no id table at all (ScratchBank has no id storage) -- always
	// NAME_None there, a real, defined, disclosed degrade to "#index"-only addressing
	// (the class comment's documented asymmetry), never a silent wrong answer.
	return Kind == EKind::Asset && Asset.IsValid() ? Asset->GetIdForIndex(Index) : NAME_None;
}

int32 FSuperFAISSInspectionSource::GetIndexForId(FName Id) const
{
	return Kind == EKind::Asset && Asset.IsValid() ? Asset->GetIndexForId(Id) : INDEX_NONE;
}

superfaiss::BankView FSuperFAISSInspectionSource::GetBankView() const
{
	switch (Kind)
	{
	case EKind::Asset:
		return Asset.IsValid() ? Asset->GetBankView() : superfaiss::BankView();
	case EKind::Archive:
	{
		superfaiss::BankView View;
		if (ArchiveBank.IsValid())
		{
			const int32 Words = superfaiss::ScratchBank::TombstoneWords(ArchiveBank->GetCount());
			TArray<uint32> Scratch;
			Scratch.SetNumZeroed(FMath::Max(Words, 1));
			ArchiveBank->Core().Snapshot(&View, Scratch.GetData());
		}
		return View;
	}
	default:
		return superfaiss::BankView();
	}
}

TArray<uint32> FSuperFAISSInspectionSource::GetTombstoneWords() const
{
	// The space law's other half (section 25.3): always empty for an asset (no
	// tombstones exist there -- the documented asymmetry); the scratch bank's own
	// Snapshot() words for an archive, real and correctly-sized even when every row is
	// live (an all-zero bitset, never an absent one).
	TArray<uint32> Words;
	if (Kind == EKind::Archive && ArchiveBank.IsValid())
	{
		const int32 Count = ArchiveBank->GetCount();
		const int32 WordCount = superfaiss::ScratchBank::TombstoneWords(Count);
		TArray<uint32> Scratch;
		Scratch.SetNumZeroed(FMath::Max(WordCount, 1));
		superfaiss::BankView View;
		ArchiveBank->Core().Snapshot(&View, Scratch.GetData());
		Words = MoveTemp(Scratch);
		Words.SetNum(WordCount);
	}
	return Words;
}

void SSuperFAISSBankInspector::Construct(const FArguments& InArgs)
{
	RefreshBankList();
	const FSlateFontInfo Body = FCoreStyle::GetDefaultFontStyle("Regular", 11);

	ChildSlot
	[
		SNew(SBorder).Padding(12.0f)
		[
			SNew(SHorizontalBox)

			// Left: inspector.
			+ SHorizontalBox::Slot().FillWidth(0.5f).Padding(0, 0, 8, 0)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
				[
					SNew(SComboBox<TSharedPtr<FString>>)
					.OptionsSource(&BankNames)
					.OnGenerateWidget_Lambda([Body](TSharedPtr<FString> Item)
					{
						return SNew(STextBlock).Font(Body).Text(FText::FromString(*Item));
					})
					.OnSelectionChanged_Lambda(
						[this](TSharedPtr<FString> Item, ESelectInfo::Type)
						{
							SelectedBankName = Item;
							OnBankSelected();
						})
					[
						SNew(STextBlock).Font(Body)
						.Text_Lambda([this]()
						{
							return FText::FromString(SelectedBankName.IsValid()
								? *SelectedBankName : TEXT("select a bank..."));
						})
					]
				]
				// Channel table + per-channel recall + memory accounting, one line.
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
				[
					SNew(STextBlock).Font(Body).AutoWrapText(true)
					.Text_Lambda([this]() { return FText::FromString(BankInfoLine()); })
				]
				// Channel weight sliders (channel banks only).
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
				[
					SAssignNew(ChannelSliderBox, SVerticalBox)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
				[
					SNew(SEditableTextBox).Font(Body)
					.HintText(FText::FromString(
						TEXT("row id (banks with ids) or #index — ranked matches with margins")))
					.OnTextCommitted_Lambda(
						[this](const FText& Text, ETextCommit::Type Commit)
						{
							if (Commit == ETextCommit::OnEnter)
							{
								RunQuery(Text.ToString());
							}
						})
				]
				// View B (Novelty probe), section 25.5: "a probe slot on the existing
				// query pane" -- same row-id/#index parse as the query box above.
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 2)
				[
					SNew(SEditableTextBox).Font(Body)
					.HintText(FText::FromString(
						TEXT("novelty probe: row id or #index — is this direction novel?")))
					.OnTextCommitted_Lambda(
						[this](const FText& Text, ETextCommit::Type Commit)
						{
							if (Commit == ETextCommit::OnEnter)
							{
								ProbeNovelty(Text.ToString());
								if (NoveltyEvidenceList.IsValid())
								{
									NoveltyEvidenceList->RequestListRefresh();
								}
							}
						})
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 2)
				[
					SNew(STextBlock).Font(Body).AutoWrapText(true)
					.Text_Lambda([this]() { return FText::FromString(BuildNoveltyVerdictText()); })
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
				[
					SNew(SBox).HeightOverride(90.0f)
					[
						SNew(SBorder).Padding(2.0f)
						[
							SAssignNew(NoveltyEvidenceList, SListView<TSharedPtr<FString>>)
							.ListItemsSource(&NoveltyEvidenceLines)
							.OnGenerateRow_Lambda([Body](TSharedPtr<FString> Item,
								const TSharedRef<STableViewBase>& Owner)
							{
								return SNew(STableRow<TSharedPtr<FString>>, Owner)
								[
									SNew(STextBlock).Font(Body).Text(FText::FromString(*Item))
								];
							})
						]
					]
				]
				+ SVerticalBox::Slot().FillHeight(1.0f)
				[
					SAssignNew(ResultList, SListView<TSharedPtr<FString>>)
					.ListItemsSource(&ResultLines)
					.OnGenerateRow_Lambda([Body](TSharedPtr<FString> Item,
						const TSharedRef<STableViewBase>& Owner)
					{
						return SNew(STableRow<TSharedPtr<FString>>, Owner)
						[
							SNew(STextBlock).Font(Body).Text(FText::FromString(*Item))
						];
					})
				]
			]

			// Right: projection visualizer + View A (Structure).
			+ SHorizontalBox::Slot().FillWidth(0.5f).Padding(8, 0, 0, 0)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.OnClicked_Lambda([this]()
						{
							ComputeProjection();
							return FReply::Handled();
						})
						[
							SNew(STextBlock).Font(Body)
							.Text(FText::FromString(TEXT("Compute projection (PCA)")))
						]
					]
					// View A (Structure), section 25.5: "beside 'Compute projection (PCA)'".
					+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0, 0, 0)
					[
						SNew(SButton)
						.OnClicked_Lambda([this]()
						{
							ComputeStructure();
							RebuildStructureClusterList();
							return FReply::Handled();
						})
						[
							SNew(STextBlock).Font(Body)
							.Text(FText::FromString(TEXT("Compute structure")))
						]
					]
					// Channel scope for the projection AND View A/B (channel banks only;
					// the class-header design note: one shared analysis-scope selector).
					+ SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0)
					[
						SNew(SComboBox<TSharedPtr<FString>>)
						.OptionsSource(&ProjectionScopes)
						.OnGenerateWidget_Lambda([Body](TSharedPtr<FString> Item)
						{
							return SNew(STextBlock).Font(Body).Text(FText::FromString(*Item));
						})
						.OnSelectionChanged_Lambda(
							[this](TSharedPtr<FString> Item, ESelectInfo::Type)
							{
								SelectedProjectionScope = Item;
								ProjectedPoints.Reset();
								ProjectionStatus.Reset();
								// Section 25.9 dim 1: a scope change invalidates the
								// Structure/Novelty caches too, not only the
								// projection (V32-G2 discipline extended). Also
								// clears the cluster list/highlight (UI-only).
								InvalidateAnalysisCaches();
							})
						[
							SNew(STextBlock).Font(Body)
							.Text_Lambda([this]()
							{
								return FText::FromString(SelectedProjectionScope.IsValid()
									? *SelectedProjectionScope : TEXT("(whole row)"));
							})
						]
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(10, 4, 0, 0)
					[
						SNew(STextBlock).Font(Body)
						.Text_Lambda([this]()
						{
							return FText::FromString(ProjectionStatus);
						})
					]
				]
				// View A disclosure copy + status (section 25.5, E-D3-1).
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 2)
				[
					SNew(STextBlock).Font(Body).AutoWrapText(true)
					.Text(FText::FromString(StructureDisclosureCopy()))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 2)
				[
					SNew(STextBlock).Font(Body).AutoWrapText(true)
					.Text_Lambda([this]() { return FText::FromString(StructureStatus); })
				]
				// Cluster tree (section 25.5): one header per cluster ("Component N: K
				// members" — the header's own id is a canonical internal integer, not a
				// meaningful name) + an Outliers header, each expandable to its member
				// rows by real bank id (or #index). Selecting a header highlights the
				// whole cluster; selecting a member row highlights it + its k nearest.
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
				[
					SNew(SBox).HeightOverride(180.0f)
					[
						SNew(SBorder).Padding(2.0f)
						[
							SAssignNew(StructureClusterTree,
								STreeView<TSharedPtr<FSuperFAISSStructureListItem>>)
							.TreeItemsSource(&StructureListRoots)
							.OnGenerateRow_Lambda([Body](TSharedPtr<FSuperFAISSStructureListItem> Item,
								const TSharedRef<STableViewBase>& Owner)
							{
								return SNew(STableRow<TSharedPtr<FSuperFAISSStructureListItem>>, Owner)
								[
									SNew(STextBlock).Font(Body)
									.Text(FText::FromString(Item.IsValid() ? Item->DisplayText : FString()))
								];
							})
							.OnGetChildren(this, &SSuperFAISSBankInspector::GetStructureItemChildren)
							.OnSelectionChanged_Lambda(
								[this](TSharedPtr<FSuperFAISSStructureListItem> Item, ESelectInfo::Type)
								{
									OnStructureItemSelected(Item);
								})
						]
					]
				]
				// View C (Correspondence), section 25.5: the second-bank slot + "Compute
				// correspondence" trigger + the matched-pair list. The state column is
				// TEXT ONLY (matched / ambiguous / unmatched) — never color-coded rows
				// (section 25.5, verbatim), so the row widget below carries no per-state
				// background paint.
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 2)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.0f)
					[
						SNew(SComboBox<TSharedPtr<FString>>)
						.OptionsSource(&SecondBankNames)
						.OnGenerateWidget_Lambda([Body](TSharedPtr<FString> Item)
						{
							return SNew(STextBlock).Font(Body).Text(FText::FromString(*Item));
						})
						.OnSelectionChanged_Lambda(
							[this](TSharedPtr<FString> Item, ESelectInfo::Type)
							{
								SelectedSecondBankName = Item;
								OnSecondBankSelected();
							})
						[
							SNew(STextBlock).Font(Body)
							.Text_Lambda([this]()
							{
								return FText::FromString(SelectedSecondBankName.IsValid()
									? *SelectedSecondBankName : TEXT("select a second bank..."));
							})
						]
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0, 0, 0)
					[
						SNew(SButton)
						.OnClicked_Lambda([this]()
						{
							ComputeCorrespondence();
							if (MatchPairList.IsValid())
							{
								MatchPairList->RequestListRefresh();
							}
							return FReply::Handled();
						})
						[
							SNew(STextBlock).Font(Body)
							.Text(FText::FromString(TEXT("Compute correspondence")))
						]
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 2)
				[
					SNew(STextBlock).Font(Body).AutoWrapText(true)
					.Text_Lambda([this]() { return FText::FromString(CorrespondenceStatus); })
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
				[
					SNew(SBox).HeightOverride(120.0f)
					[
						SNew(SBorder).Padding(2.0f)
						[
							SAssignNew(MatchPairList, SListView<TSharedPtr<FString>>)
							.ListItemsSource(&MatchPairDisplayLines)
							.OnGenerateRow_Lambda([Body](TSharedPtr<FString> Item,
								const TSharedRef<STableViewBase>& Owner)
							{
								// Text-only row (no per-state background) — the "never
								// color-coded rows" contract, section 25.5.
								return SNew(STableRow<TSharedPtr<FString>>, Owner)
								[
									SNew(STextBlock).Font(Body).Text(FText::FromString(*Item))
								];
							})
						]
					]
				]
				+ SVerticalBox::Slot().FillHeight(1.0f)
				[
					SNew(SBorder).Padding(2.0f)
					[
						SNew(SSuperFAISSScatter).Points(&ProjectedPoints)
						.ComponentIds(&StructureComponentIdBySampleIndex)
						.HighlightedSampleIndices(&HighlightedSampleIndices)
						.GetPointLabel(TFunction<FText(int32)>(
							[this](int32 SampleIdx) { return GetScatterPointLabel(SampleIdx); }))
					]
				]
			]
		]
	];

	RefreshSecondBankList();
	OnBankSelected();
}

void SSuperFAISSBankInspector::RefreshBankList()
{
	BankNames.Reset();
	BankAssets.Reset();
	FAssetRegistryModule& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	TArray<FAssetData> Assets;
	AssetRegistry.Get().GetAssetsByClass(
		USuperFAISSVectorBank::StaticClass()->GetClassPathName(), Assets);
	for (const FAssetData& Asset : Assets)
	{
		if (USuperFAISSVectorBank* Bank = Cast<USuperFAISSVectorBank>(Asset.GetAsset()))
		{
			BankNames.Add(MakeShared<FString>(Bank->GetName()));
			BankAssets.Add(Bank);
		}
	}
	if (BankNames.Num() > 0)
	{
		SelectedBankName = BankNames[0];
	}
}

USuperFAISSVectorBank* SSuperFAISSBankInspector::GetSelectedBank() const
{
	for (int32 i = 0; i < BankNames.Num(); ++i)
	{
		if (BankNames[i] == SelectedBankName && BankAssets.IsValidIndex(i))
		{
			return BankAssets[i].Get();
		}
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// V3.2 plan section 25.5 View C (Correspondence), slot 4 — the second-bank slot
// (section 25.3 E-D1-1..4). Mirrors RefreshBankList()/GetSelectedBank() exactly; this
// list population and lookup are pre-existing, shipped machinery (asset-registry
// enumeration) repointed at a second slot, not new achievement — real, not RED
// SCAFFOLD, same reasoning slot 3 applied to the channel-slider/scope-combo population.
// ---------------------------------------------------------------------------

void SSuperFAISSBankInspector::RefreshSecondBankList()
{
	SecondBankNames.Reset();
	SecondBankAssets.Reset();
	FAssetRegistryModule& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	TArray<FAssetData> Assets;
	AssetRegistry.Get().GetAssetsByClass(
		USuperFAISSVectorBank::StaticClass()->GetClassPathName(), Assets);
	for (const FAssetData& Asset : Assets)
	{
		if (USuperFAISSVectorBank* Bank = Cast<USuperFAISSVectorBank>(Asset.GetAsset()))
		{
			SecondBankNames.Add(MakeShared<FString>(Bank->GetName()));
			SecondBankAssets.Add(Bank);
		}
	}
}

USuperFAISSVectorBank* SSuperFAISSBankInspector::GetSelectedSecondBank() const
{
	for (int32 i = 0; i < SecondBankNames.Num(); ++i)
	{
		if (SecondBankNames[i] == SelectedSecondBankName && SecondBankAssets.IsValidIndex(i))
		{
			return SecondBankAssets[i].Get();
		}
	}
	return nullptr;
}

void SSuperFAISSBankInspector::OnSecondBankSelected()
{
	// Slot 4b mutual exclusion (FSuperFAISSInspectionSource's class comment): picking an
	// asset from the second-bank combo supersedes any open second archive.
	SecondArchiveBank.Reset();
	SecondArchiveDisplayName.Reset();
	InvalidateAnalysisCaches();
}

bool SSuperFAISSBankInspector::CheckSecondBankCompatible(const USuperFAISSVectorBank& A,
	const USuperFAISSVectorBank* B, FString& OutReason) const
{
	if (B == nullptr)
	{
		OutReason = TEXT("no second bank selected");
		return false;
	}
	if (!B->IsValid())
	{
		OutReason = TEXT("second bank: invalid asset");
		return false;
	}
	if (A.Dims != B->Dims)
	{
		OutReason = TEXT("second bank: dims mismatch");
		return false;
	}
	if (A.Metric != B->Metric)
	{
		OutReason = TEXT("second bank: metric mismatch");
		return false;
	}
	return true;
}

bool SSuperFAISSBankInspector::CheckSecondBankCompatible(const FSuperFAISSInspectionSource& A,
	const FSuperFAISSInspectionSource& B, FString& OutReason) const
{
	if (B.Kind == FSuperFAISSInspectionSource::EKind::None)
	{
		OutReason = TEXT("no second bank selected");
		return false;
	}
	if (!B.IsValid())
	{
		OutReason = TEXT("second bank: invalid asset");
		return false;
	}
	if (A.GetDims() != B.GetDims())
	{
		OutReason = TEXT("second bank: dims mismatch");
		return false;
	}
	if (A.GetMetric() != B.GetMetric())
	{
		OutReason = TEXT("second bank: metric mismatch");
		return false;
	}
	return true;
}

FSuperFAISSInspectionSource SSuperFAISSBankInspector::GetPrimarySource() const
{
	FSuperFAISSInspectionSource Source;
	if (PrimaryArchiveBank.IsValid())
	{
		Source.Kind = FSuperFAISSInspectionSource::EKind::Archive;
		Source.ArchiveBank = PrimaryArchiveBank;
		Source.ArchiveDisplayName = PrimaryArchiveDisplayName;
		return Source;
	}
	if (USuperFAISSVectorBank* Bank = GetSelectedBank())
	{
		Source.Kind = FSuperFAISSInspectionSource::EKind::Asset;
		Source.Asset = Bank;
	}
	return Source;
}

FSuperFAISSInspectionSource SSuperFAISSBankInspector::GetSecondSource() const
{
	FSuperFAISSInspectionSource Source;
	if (SecondArchiveBank.IsValid())
	{
		Source.Kind = FSuperFAISSInspectionSource::EKind::Archive;
		Source.ArchiveBank = SecondArchiveBank;
		Source.ArchiveDisplayName = SecondArchiveDisplayName;
		return Source;
	}
	if (USuperFAISSVectorBank* Bank = GetSelectedSecondBank())
	{
		Source.Kind = FSuperFAISSInspectionSource::EKind::Asset;
		Source.Asset = Bank;
	}
	return Source;
}

// ---------------------------------------------------------------------------
// The "Open scratch archive..." affordance. Real, mechanical wiring of already-real,
// already-shipped primitives (USuperFAISSScratchBank::LoadFromBytes -> core
// ScratchBank::Load, reject-over-degrade) -- no algorithm invented here, the same
// "mechanical scaffolding authored real" posture already applied to
// CheckSecondBankCompatible/RefreshSecondBankList. The crux work this leaves
// deliberately unbuilt lives downstream, in BuildAnalysisSample(Source, ...).
// ---------------------------------------------------------------------------

bool SSuperFAISSBankInspector::OpenScratchArchiveFromBytes(const TArray<uint8>& Bytes, const FString& DisplayName)
{
	USuperFAISSScratchBank* Loaded = NewObject<USuperFAISSScratchBank>();
	if (!Loaded->LoadFromBytes(Bytes))
	{
		// Core Load's own reject-over-degrade contract, surfaced (the dim-2 idiom): every
		// existing source (an asset, or a previously-open archive) is left EXACTLY as it
		// was -- nothing below this line runs on the failure path.
		ArchiveOpenStatus = TEXT("archive: bad format (corrupt, truncated, wrong version, or not an archive)");
		return false;
	}
	PrimaryArchiveBank = TStrongObjectPtr<USuperFAISSScratchBank>(Loaded);
	PrimaryArchiveDisplayName = DisplayName;
	// Mutual exclusion (FSuperFAISSInspectionSource's class comment): an opened archive
	// supersedes the asset-registry combo selection.
	SelectedBankName.Reset();
	ArchiveOpenStatus.Reset();
	// The NEW archive-swap leg of the reset matrix (audit F3): opening an archive is a
	// selection-change event exactly like OnBankSelected()'s own trigger, so it fires the
	// SAME one-rule cache clear -- archive #1's exclusion/tombstone state and live-row
	// sample never survive into archive #2's passes.
	InvalidateAnalysisCaches();
	return true;
}

bool SSuperFAISSBankInspector::OpenSecondScratchArchiveFromBytes(const TArray<uint8>& Bytes, const FString& DisplayName)
{
	USuperFAISSScratchBank* Loaded = NewObject<USuperFAISSScratchBank>();
	if (!Loaded->LoadFromBytes(Bytes))
	{
		SecondArchiveOpenStatus = TEXT("archive: bad format (corrupt, truncated, wrong version, or not an archive)");
		return false;
	}
	SecondArchiveBank = TStrongObjectPtr<USuperFAISSScratchBank>(Loaded);
	SecondArchiveDisplayName = DisplayName;
	SelectedSecondBankName.Reset();
	SecondArchiveOpenStatus.Reset();
	InvalidateAnalysisCaches();
	return true;
}

void SSuperFAISSBankInspector::OnBankSelected()
{
	// Slot 4b mutual exclusion (FSuperFAISSInspectionSource's class comment): picking an
	// asset from the primary combo supersedes any open primary archive.
	PrimaryArchiveBank.Reset();
	PrimaryArchiveDisplayName.Reset();
	ProjectedPoints.Reset();
	ProjectionStatus.Reset();
	// Section 25.3 "Cache lifetime is per-widget-session, invalidated on selection
	// change": Structure/Novelty follow the same reset the projection already gets.
	InvalidateAnalysisCaches();

	// Channel state follows the selection: one unit-weight slider per channel;
	// projection scopes are the whole row plus each named channel.
	ChannelWeights.Reset();
	ChannelSliderNames.Reset();
	ProjectionScopes.Reset();
	ProjectionScopes.Add(MakeShared<FString>(TEXT("(whole row)")));
	SelectedProjectionScope = ProjectionScopes[0];
	if (const USuperFAISSVectorBank* Bank = GetSelectedBank())
	{
		for (const FName& Channel : Bank->ChannelNames)
		{
			ChannelSliderNames.Add(MakeShared<FString>(Channel.ToString()));
			ChannelWeights.Add(1.0f);
			ProjectionScopes.Add(MakeShared<FString>(Channel.ToString()));
		}
	}
	RebuildChannelSliders();
}

void SSuperFAISSBankInspector::RebuildChannelSliders()
{
	if (!ChannelSliderBox.IsValid())
	{
		return;
	}
	ChannelSliderBox->ClearChildren();
	const FSlateFontInfo Body = FCoreStyle::GetDefaultFontStyle("Regular", 11);
	for (int32 C = 0; C < ChannelSliderNames.Num(); ++C)
	{
		ChannelSliderBox->AddSlot().AutoHeight().Padding(0, 0, 0, 2)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBox).WidthOverride(140.0f)
				[
					SNew(STextBlock).Font(Body)
					.Text_Lambda([this, C]()
					{
						return FText::FromString(FString::Printf(TEXT("%s  %.2f"),
							ChannelSliderNames.IsValidIndex(C)
								? **ChannelSliderNames[C] : TEXT("?"),
							ChannelWeights.IsValidIndex(C) ? ChannelWeights[C] : 0.0f));
					})
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				// Slider range [0, 2]: 1.0 is the bank's own balance; 0 mutes the
				// channel (omission semantics), 2 doubles its say.
				SNew(SSlider)
				.MinValue(0.0f).MaxValue(2.0f)
				.Value_Lambda([this, C]()
				{
					return ChannelWeights.IsValidIndex(C) ? ChannelWeights[C] : 1.0f;
				})
				.OnValueChanged_Lambda([this, C](float Value)
				{
					if (ChannelWeights.IsValidIndex(C))
					{
						ChannelWeights[C] = Value;
					}
				})
			]
		];
	}
}

FString SSuperFAISSBankInspector::BankInfoLine() const
{
	const USuperFAISSVectorBank* Bank = GetSelectedBank();
	if (Bank == nullptr || !Bank->IsValid())
	{
		return FString();
	}
	if (Bank->GetChannelCount() == 0)
	{
		return FString::Printf(TEXT("%d x %d, schema %d, no channels"),
			Bank->Count, Bank->Dims, Bank->SchemaVersion);
	}
	// Channel table with per-channel memory accounting and recall where measured.
	const int32 ElemBytes =
		Bank->Quantization == ESuperFAISSBankQuantization::Int8 ? 1 : 4;
	FString Line = FString::Printf(TEXT("%d x %d, schema %d — "),
		Bank->Count, Bank->Dims, Bank->SchemaVersion);
	for (int32 C = 0; C < Bank->GetChannelCount(); ++C)
	{
		const int64 Bytes =
			static_cast<int64>(Bank->Count) * Bank->ChannelLengths[C] * ElemBytes;
		Line += FString::Printf(TEXT("%s%s[%d..%d) %.1f KB"),
			C > 0 ? TEXT(", ") : TEXT(""),
			*Bank->ChannelNames[C].ToString(),
			Bank->ChannelOffsets[C],
			Bank->ChannelOffsets[C] + Bank->ChannelLengths[C],
			Bytes / 1024.0);
		if (Bank->ChannelRecallAt10.IsValidIndex(C))
		{
			Line += FString::Printf(TEXT(" r@10 %.3f"), Bank->ChannelRecallAt10[C]);
		}
	}
	if (Bank->CrossDeviceRecallAt10 >= 0.0f)
	{
		Line += FString::Printf(TEXT(" — cross-device r@10 %.3f"),
			Bank->CrossDeviceRecallAt10);
	}
	return Line;
}

FText SSuperFAISSBankInspector::GetScatterPointLabel(int32 SampleIndex) const
{
	const USuperFAISSVectorBank* Bank = GetSelectedBank();
	FString RowLabel;
	if (Bank != nullptr && StructureSampleSourceIndices.IsValidIndex(SampleIndex))
	{
		const int32 SourceRow = StructureSampleSourceIndices[SampleIndex];
		const FName Id = Bank->GetIdForIndex(SourceRow);
		RowLabel = Id.IsNone() ? FString::Printf(TEXT("#%d"), SourceRow) : Id.ToString();
	}
	else
	{
		// Neither Compute projection nor Compute structure has populated the sample ->
		// source-row map yet; fall back to the bare sample position rather than nothing.
		RowLabel = FString::Printf(TEXT("sample #%d"), SampleIndex);
	}

	FString ClusterLabel;
	if (StructureComponentIdBySampleIndex.IsValidIndex(SampleIndex))
	{
		const int32 ComponentId = StructureComponentIdBySampleIndex[SampleIndex];
		if (const FSuperFAISSStructureCluster* Cluster = FindStructureCluster(ComponentId))
		{
			ClusterLabel = FString::Printf(
				TEXT("\nComponent %d (%d members)"), ComponentId, Cluster->MemberSampleIndices.Num());
		}
		else if (StructureOutlierSampleIndices.Contains(SampleIndex))
		{
			ClusterLabel = TEXT("\nOutlier");
		}
	}
	return FText::FromString(RowLabel + ClusterLabel);
}

const FSuperFAISSStructureCluster* SSuperFAISSBankInspector::FindStructureCluster(int32 ComponentId) const
{
	if (ComponentId < 0)
	{
		return nullptr;
	}
	for (const FSuperFAISSStructureCluster& Cluster : StructureClusters)
	{
		if (Cluster.ComponentId == ComponentId)
		{
			return &Cluster;
		}
	}
	return nullptr;
}

void SSuperFAISSBankInspector::RunQuery(const FString& Text)
{
	ResultLines.Reset();
	USuperFAISSVectorBank* Bank = GetSelectedBank();
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (Bank == nullptr || !Bank->IsValid() || Subsystem == nullptr)
	{
		ResultLines.Add(MakeShared<FString>(TEXT("no valid bank selected")));
	}
	else
	{
		int32 Row = INDEX_NONE;
		FString Trimmed = Text.TrimStartAndEnd();
		if (Trimmed.StartsWith(TEXT("#")))
		{
			Row = FCString::Atoi(*Trimmed.Mid(1));
		}
		else
		{
			Row = Bank->GetIndexForId(FName(*Trimmed));
		}
		TArray<float> Query;
		if (Row < 0 || Row >= Bank->Count ||
			!Subsystem->MakeCentroidQuery(Bank, {Row}, Query))
		{
			ResultLines.Add(MakeShared<FString>(
				FString::Printf(TEXT("row not found: %s"), *Trimmed)));
		}
		else
		{
			FSuperFAISSQueryArgs Args;
			Args.K = 12;
			// Channel banks query by name with the slider weights — the same list
			// DecomposeHit explains below.
			const bool bChannels = Bank->GetChannelCount() > 0 &&
				ChannelWeights.Num() == Bank->GetChannelCount();
			if (bChannels)
			{
				for (int32 C = 0; C < Bank->GetChannelCount(); ++C)
				{
					Args.Channels.Add({Bank->ChannelNames[C], ChannelWeights[C]});
				}
			}
			TArray<FSuperFAISSHit> Hits;
			if (!Subsystem->QuerySync(Bank, Query, Args, Hits))
			{
				ResultLines.Add(MakeShared<FString>(TEXT("query failed")));
			}
			for (const FSuperFAISSHit& Hit : Hits)
			{
				FString Line = FString::Printf(
					TEXT("%-24s  score %.4f   margin %.4f"),
					Hit.Id.IsNone() ? *FString::Printf(TEXT("#%d"), Hit.Index)
					                : *Hit.Id.ToString(),
					Hit.Score, Hit.Margin);

				// Decomposition bars: contributions sum exactly to the score.
				TArray<float> Contributions;
				float Total = 0.0f;
				if (bChannels && Subsystem->DecomposeHit(Bank, Query, Args.Channels,
						Hit.Index, Contributions, Total))
				{
					float MaxAbs = 0.0f;
					for (const float V : Contributions)
					{
						MaxAbs = FMath::Max(MaxAbs, FMath::Abs(V));
					}
					for (int32 C = 0; C < Contributions.Num(); ++C)
					{
						// The per-channel cosine is contribution/weight; displayed
						// values clamp to [-1, 1] (T-044 W2d — int8 noise can push a
						// shade past unit; display-only, '*' marks a clamp).
						const float Weight = Args.Channels[C].Weight;
						FString Cosine;
						if (Weight != 0.0f &&
							Bank->Metric == ESuperFAISSBankMetric::Cosine)
						{
							const float Raw = Contributions[C] / Weight;
							const float Clamped = FMath::Clamp(Raw, -1.0f, 1.0f);
							Cosine = FString::Printf(TEXT(" cos %.3f%s"), Clamped,
								Raw != Clamped ? TEXT("*") : TEXT(""));
						}
						Line += FString::Printf(TEXT("  | %s %-10s %+.4f%s"),
							*Args.Channels[C].Channel.ToString(),
							*ContributionBar(Contributions[C], MaxAbs),
							Contributions[C], *Cosine);
					}
				}
				ResultLines.Add(MakeShared<FString>(MoveTemp(Line)));
			}
		}
	}
	if (ResultList.IsValid())
	{
		ResultList->RequestListRefresh();
	}
}

void SSuperFAISSBankInspector::ComputeProjection()
{
	using namespace superfaiss;

	ProjectedPoints.Reset();
	// The source generalization -- an asset OR an open archive, resolved uniformly (the
	// SAME repointing ComputeStructure()/ComputeCorrespondence() already use; this is a
	// pure BankView-native pass that already rides BuildAnalysisSample, so it carries none
	// of ProbeNovelty's asset-typed-subsystem obstacle -- a plain GetSelectedBank() ->
	// GetPrimarySource() substitution).
	const FSuperFAISSInspectionSource Source = GetPrimarySource();
	if (!Source.IsValid())
	{
		ProjectionStatus = TEXT("no valid bank selected");
		return;
	}
	// "One shared cap" / "same sample": the projection now rides the SAME
	// BuildAnalysisSample construction View A/B's baseline does, keyed by the same
	// USuperFAISSInspectorSettings
	// SampleLimit — the header's "1:1 scatter join" claim (StructureComponentIdBySampleIndex
	// parallel to ProjectedPoints) is genuinely true, not merely numerically coincidental at
	// the shared default. Previously this function ran its own independent stride sampler
	// (ProjectionSampleLimit) that diverged from Structure's sample above the cap in both
	// length and source-row set.
	const USuperFAISSInspectorSettings* Settings = GetDefault<USuperFAISSInspectorSettings>();
	TArray<uint8, TAlignedHeapAllocator<16>> Payload;
	TArray<float> Scales;
	BankView View;
	TArray<int32> Sampled;
	if (!BuildAnalysisSample(Source, Settings->GetClampedSampleLimit(), Payload, Scales, View, Sampled))
	{
		ProjectionStatus = TEXT("unknown channel scope");
		return;
	}
	const FString ScopeLabel = (SelectedProjectionScope.IsValid() &&
		*SelectedProjectionScope != TEXT("(whole row)"))
		? FString::Printf(TEXT(", channel %s"), **SelectedProjectionScope)
		: FString();
	const bool bSampled = View.count < Source.GetCount();

	TArray<float> Mean, Components, Scratch, Coords;
	Mean.SetNumUninitialized(View.dims);
	Components.SetNumUninitialized(2 * View.dims);
	Scratch.SetNumUninitialized(View.dims);
	Coords.SetNumUninitialized(View.count * 2);
	if (ComputePrincipalComponents(View, 2, PcaIterations, Mean.GetData(),
			Components.GetData(), Scratch.GetData()) != Status::Ok ||
		ProjectRowsOntoComponents(View, Mean.GetData(), Components.GetData(), 2,
			Coords.GetData()) != Status::Ok)
	{
		ProjectionStatus = TEXT("projection failed");
		return;
	}

	// Normalize into [0,1] for the paint pass.
	FVector2f Min(FLT_MAX, FLT_MAX), Max(-FLT_MAX, -FLT_MAX);
	for (int32 P = 0; P < View.count; ++P)
	{
		Min.X = FMath::Min(Min.X, Coords[P * 2]);
		Min.Y = FMath::Min(Min.Y, Coords[P * 2 + 1]);
		Max.X = FMath::Max(Max.X, Coords[P * 2]);
		Max.Y = FMath::Max(Max.Y, Coords[P * 2 + 1]);
	}
	const FVector2f Span(FMath::Max(Max.X - Min.X, KINDA_SMALL_NUMBER),
		FMath::Max(Max.Y - Min.Y, KINDA_SMALL_NUMBER));
	ProjectedPoints.Reserve(View.count);
	for (int32 P = 0; P < View.count; ++P)
	{
		ProjectedPoints.Add(FVector2f((Coords[P * 2] - Min.X) / Span.X,
			(Coords[P * 2 + 1] - Min.Y) / Span.Y));
	}
	ProjectionStatus = FString::Printf(TEXT("%d of %d rows%s%s"), View.count, Source.GetCount(),
		bSampled ? TEXT(" (sampled)") : TEXT(""), *ScopeLabel);

	// Also persisted here, not only in ComputeStructure(): both functions share the exact
	// same BuildAnalysisSample construction (F2 fix), so this stays valid for hover-label
	// resolution whether the user has run Compute structure yet or only Compute projection.
	StructureSampleSourceIndices = Sampled;
}

// ---------------------------------------------------------------------------
// View A (Structure) / View B (Novelty probe). Built against graph.h/novelty.h
// (vendored into this plugin's ThirdParty tree, plus a contract-cleanliness fix,
// reviewed as built code). The disclosure-copy accessors are unchanged: fixed
// literal strings, not achievement.
// ---------------------------------------------------------------------------

void SSuperFAISSBankInspector::InvalidateAnalysisCaches()
{
	StructureClusters.Reset();
	StructureOutlierSampleIndices.Reset();
	StructureComponentIdBySampleIndex.Reset();
	StructureStatus.Reset();
	StructureSampleSourceIndices.Reset();
	StructureNeighborSampleIndices.Reset();
	StructureNeighborK = 0;
	NoveltyResult = FSuperFAISSNoveltyResult();
	NoveltyEvidenceLines.Reset();
	NoveltyStatus.Reset();
	NoveltyBaselineSortedDistances.Reset();
	bNoveltyBaselineCalibrated = false;
	// View C (slot 4): the SAME coarse rule extends to Correspondence (audit N-2's
	// "structure + baseline + matches, one rule for every panel").
	MatchPairResults.Reset();
	MatchPairDisplayLines.Reset();
	CorrespondenceStatus.Reset();
	RebuildStructureClusterList(); // UI-only, but stale iff StructureClusters is
	// Slot 4b (audit F3): this SAME one-rule clear is now also reached by
	// OpenScratchArchiveFromBytes()/OpenSecondScratchArchiveFromBytes() on a successful
	// open — the NEW archive-swap leg of the reset matrix. Archive #1's exclusion/
	// tombstone state and live-row sample never survive into archive #2's passes because
	// there is no separate "archive" path here at all: opening an archive is just another
	// selection-change event routed through this same function, exactly like
	// OnBankSelected()/OnSecondBankSelected()/a scope change.
}

void SSuperFAISSBankInspector::RebuildStructureClusterList()
{
	StructureListRoots.Reset();
	HighlightedSampleIndices.Reset();

	const USuperFAISSVectorBank* Bank = GetSelectedBank();
	const auto MemberLabel = [this, Bank](int32 SampleIdx) -> FString
	{
		if (Bank == nullptr || !StructureSampleSourceIndices.IsValidIndex(SampleIdx))
		{
			return FString::Printf(TEXT("#%d"), SampleIdx);
		}
		const int32 SourceRow = StructureSampleSourceIndices[SampleIdx];
		const FName Id = Bank->GetIdForIndex(SourceRow);
		return Id.IsNone() ? FString::Printf(TEXT("#%d"), SourceRow) : Id.ToString();
	};
	const auto MakeLeaf = [&MemberLabel](int32 SampleIdx) -> TSharedPtr<FSuperFAISSStructureListItem>
	{
		TSharedPtr<FSuperFAISSStructureListItem> Leaf = MakeShared<FSuperFAISSStructureListItem>();
		Leaf->DisplayText = MemberLabel(SampleIdx);
		Leaf->SampleIndex = SampleIdx;
		return Leaf;
	};

	for (const FSuperFAISSStructureCluster& Cluster : StructureClusters)
	{
		TSharedPtr<FSuperFAISSStructureListItem> Header = MakeShared<FSuperFAISSStructureListItem>();
		Header->DisplayText = FString::Printf(
			TEXT("Component %d: %d members"), Cluster.ComponentId, Cluster.MemberSampleIndices.Num());
		Header->HeaderMemberSampleIndices = Cluster.MemberSampleIndices;
		for (const int32 SampleIdx : Cluster.MemberSampleIndices)
		{
			Header->Children.Add(MakeLeaf(SampleIdx));
		}
		StructureListRoots.Add(Header);
	}
	if (StructureOutlierSampleIndices.Num() > 0)
	{
		TSharedPtr<FSuperFAISSStructureListItem> OutlierHeader = MakeShared<FSuperFAISSStructureListItem>();
		OutlierHeader->DisplayText =
			FString::Printf(TEXT("Outliers: %d"), StructureOutlierSampleIndices.Num());
		OutlierHeader->HeaderMemberSampleIndices = StructureOutlierSampleIndices;
		for (const int32 SampleIdx : StructureOutlierSampleIndices)
		{
			OutlierHeader->Children.Add(MakeLeaf(SampleIdx));
		}
		StructureListRoots.Add(OutlierHeader);
	}

	if (StructureClusterTree.IsValid())
	{
		StructureClusterTree->RequestTreeRefresh();
	}
}

void SSuperFAISSBankInspector::OnStructureItemSelected(TSharedPtr<FSuperFAISSStructureListItem> Item)
{
	HighlightedSampleIndices.Reset();
	if (!Item.IsValid())
	{
		return;
	}
	if (Item->SampleIndex == INDEX_NONE)
	{
		// A cluster/Outliers header: highlight every member (section 25.5 "selecting a
		// cluster highlights its points in the scatter"). All members are the same kind here —
		// there's no row/neighbor distinction to make for a header selection.
		for (const int32 SampleIdx : Item->HeaderMemberSampleIndices)
		{
			HighlightedSampleIndices.Add(SampleIdx, ESuperFAISSStructureHighlight::Component);
		}
		return;
	}
	// A single member row: highlight ITS COMPONENT (the prior build dropped this half) plus
	// the row itself plus its k nearest ("selecting a row highlights its component + its k
	// nearest"). An outlier row legitimately has no component
	// (StructureComponentIdBySampleIndex is -1 for every outlier by construction) — row +
	// k-nearest only.
	//
	// Insertion order sets the priority (k-nearest was indistinguishable from
	// "everything in the group" once both rendered the same). TMap::Add replaces an existing
	// key's value, so Component goes in first (lowest priority, most general), Neighbor next
	// (overwrites a same-index Component tag), and the clicked row itself goes in LAST so it
	// always keeps its Selected tag even if it's also its own component member or neighbor.
	if (StructureComponentIdBySampleIndex.IsValidIndex(Item->SampleIndex))
	{
		const int32 ComponentId = StructureComponentIdBySampleIndex[Item->SampleIndex];
		if (const FSuperFAISSStructureCluster* Cluster = FindStructureCluster(ComponentId))
		{
			for (const int32 SampleIdx : Cluster->MemberSampleIndices)
			{
				HighlightedSampleIndices.Add(SampleIdx, ESuperFAISSStructureHighlight::Component);
			}
		}
	}
	if (StructureNeighborK > 0)
	{
		const int32 Base = Item->SampleIndex * StructureNeighborK;
		for (int32 j = 0; j < StructureNeighborK; ++j)
		{
			if (StructureNeighborSampleIndices.IsValidIndex(Base + j))
			{
				const int32 NeighborSample = StructureNeighborSampleIndices[Base + j];
				if (NeighborSample >= 0)
				{
					HighlightedSampleIndices.Add(NeighborSample, ESuperFAISSStructureHighlight::Neighbor);
				}
			}
		}
	}
	HighlightedSampleIndices.Add(Item->SampleIndex, ESuperFAISSStructureHighlight::Selected);
}

bool SSuperFAISSBankInspector::BuildAnalysisSample(const USuperFAISSVectorBank& Bank, int32 SampleLimit,
	TArray<uint8, TAlignedHeapAllocator<16>>& OutPayload, TArray<float>& OutScales,
	superfaiss::BankView& OutView, TArray<int32>& OutSourceIndices,
	int32* OutZeroEnergyExcludedCount, bool bCompactZeroEnergy,
	TArray<uint32>* OutZeroEnergyExcludeBits) const
{
	using namespace superfaiss;

	const BankView Full = Bank.GetBankView();

	// M1 (launch-gate finding): a non-compacting (bCompactZeroEnergy == false, "full-view
	// identity") build's zero-energy mask is indexed by CANDIDATE POSITION and sized from
	// Candidates.Num() -- this equals the native source index ONLY when SampleLimit is at
	// least Full.count, so every row is its own candidate at its own position (Finding 6's
	// documented precondition, "always called with SampleLimit == the source's own
	// published count"). That precondition was enforced nowhere: a caller passing
	// bCompactZeroEnergy=false with a SMALLER limit would silently compact anyway
	// (SampleCount = min(Full.count, SampleLimit) < Full.count), so
	// OutZeroEnergyExcludeBits and OutSourceIndices would mask the WRONG rows against
	// whatever excludeBits the caller already holds in native-index space. Refused
	// structurally, at the one call boundary where every caller of this axis passes
	// through, rather than left to a precondition a future caller could violate by
	// forgetting it.
	if (!bCompactZeroEnergy && SampleLimit < Full.count)
	{
		return false;
	}

	int32 ScopeOffset = 0;
	int32 ScopeDims = Full.dims;
	const bool bChannelScoped =
		SelectedProjectionScope.IsValid() && *SelectedProjectionScope != TEXT("(whole row)");
	if (bChannelScoped)
	{
		const int32 Channel = Bank.GetChannelIndex(FName(**SelectedProjectionScope));
		if (Channel == INDEX_NONE)
		{
			return false;
		}
		ScopeOffset = Bank.ChannelOffsets[Channel];
		ScopeDims = Bank.ChannelLengths[Channel];
	}
	// A channel slice of a whole-row-unit Cosine row is NOT itself unit-norm, and this
	// view ships with channels=nullptr/channelInvNorms=nullptr
	// (the plain Cosine kernel, which assumes the unit precondition) -- so every sliced
	// row is renormalized to unit norm OVER THE SLICE below. Whole-row scope, L2, and Dot
	// are untouched.
	const bool bRenormalizeSlice = bChannelScoped && Full.metric == Metric::Cosine;

	const int32 ElemBytes = ElementSize(Full.quant);
	const int32 ScopePd = PaddedDims(ScopeDims, Full.quant);
	const int64 ScopeRowBytes = static_cast<int64>(ScopePd) * ElemBytes;
	const int64 CopyBytes = static_cast<int64>(ScopeDims) * ElemBytes;

	// Endpoint-inclusive even sampling: exactly SampleCount entries, always
	// including source rows 0 and Full.count-1 (the general "N evenly spaced
	// points over M items, both ends included" construction).
	const int32 SampleCount = FMath::Min(Full.count, FMath::Max(1, SampleLimit));
	TArray<int32> Candidates;
	Candidates.Reserve(SampleCount);
	if (SampleCount <= 1 || Full.count <= 1)
	{
		for (int32 i = 0; i < SampleCount; ++i)
		{
			Candidates.Add(i);
		}
	}
	else
	{
		// Ceiling division, not floor: floor leaves a reachable-but-never-sampled gap
		// just before the final index for some (Full.count, SampleCount) pairs (e.g.
		// 2420 over 2048 floor-samples 2419 but skips 2418 entirely) — ceiling closes
		// it while keeping the result exactly SampleCount unique, monotonic indices.
		const int64 Denom = SampleCount - 1;
		for (int32 i = 0; i < SampleCount; ++i)
		{
			const int64 Numer = static_cast<int64>(i) * (Full.count - 1);
			Candidates.Add(static_cast<int32>((Numer + Denom - 1) / Denom));
		}
	}

	const int64 FullRowBytes = RowBytes(Full);
	OutPayload.SetNumZeroed(Candidates.Num() * ScopeRowBytes);
	OutScales.Reset();
	OutSourceIndices.Reset();
	OutSourceIndices.Reserve(Candidates.Num());
	// Finding 6 (regression on Finding 1, plugin editor review): a FULL-VIEW IDENTITY
	// build (bCompactZeroEnergy == false, always called with SampleLimit == the source's
	// own published count -- Candidates is then every row, in order) must never drop a
	// row's INDEX, or the caller's excludeBits (aligned to this same native index space)
	// land on the wrong row. A zero-energy row still cannot be renormalized -- there is no
	// direction to renormalize to -- so it stays in the view UNRENORMALIZED (its payload
	// content is never scored: the caller ORs OutZeroEnergyExcludeBits into its own
	// excludeBits before this view is ever queried) and its bit is set in
	// OutZeroEnergyExcludeBits instead of being skipped. A SAMPLE build
	// (bCompactZeroEnergy == true, the default) has no index-identity requirement --
	// OutSourceIndices IS the mapping -- so it keeps dropping zero-energy rows as before.
	if (OutZeroEnergyExcludeBits != nullptr)
	{
		OutZeroEnergyExcludeBits->Reset();
		if (!bCompactZeroEnergy)
		{
			OutZeroEnergyExcludeBits->SetNumZeroed((Candidates.Num() + 31) / 32);
		}
	}
	int32 ZeroEnergyExcludedCount = 0;
	int32 Kept = 0;
	for (int32 CandidatePos = 0; CandidatePos < Candidates.Num(); ++CandidatePos)
	{
		const int32 SrcIdx = Candidates[CandidatePos];
		const uint8* SlicePtr = static_cast<const uint8*>(Full.rows) + SrcIdx * FullRowBytes +
			static_cast<int64>(ScopeOffset) * ElemBytes;

		bool bZeroEnergy = false;
		if (bRenormalizeSlice && Full.quant == Quantization::Int8)
		{
			// DAZ-safe scale decode (bake.cpp's ComputeChannelInverseNorms convention):
			// a plain (double)scale can see a subnormal scale flushed to 0 under one
			// thread's DAZ state and preserved under another's.
			const double Scale = detail::FloatBitsToDouble(Full.scales[SrcIdx]);
			const int8_t* SliceInt8 = reinterpret_cast<const int8_t*>(SlicePtr);
			double Norm = 0.0;
			for (int32 j = 0; j < ScopeDims; ++j)
			{
				const double V = Scale * SliceInt8[j];
				Norm += V * V;
			}
			if (Norm <= 0.0)
			{
				bZeroEnergy = true;
			}
			else
			{
				const double InvNorm = 1.0 / FMath::Sqrt(Norm);
				// newScale * int8[j] is unit-norm over the slice: newScale = Scale *
				// InvNorm (the int8 bytes themselves are untouched, only the per-row
				// scale carries the renormalization).
				FMemory::Memcpy(OutPayload.GetData() + Kept * ScopeRowBytes, SlicePtr, CopyBytes);
				OutScales.Add(static_cast<float>(Scale * InvNorm));
			}
		}
		else if (bRenormalizeSlice) // Float32
		{
			const float* SliceFloat = reinterpret_cast<const float*>(SlicePtr);
			double Norm = 0.0;
			for (int32 j = 0; j < ScopeDims; ++j)
			{
				Norm += static_cast<double>(SliceFloat[j]) * SliceFloat[j];
			}
			if (Norm <= 0.0)
			{
				bZeroEnergy = true;
			}
			else
			{
				const float InvNorm = static_cast<float>(1.0 / FMath::Sqrt(Norm));
				float* DestFloat = reinterpret_cast<float*>(OutPayload.GetData() + Kept * ScopeRowBytes);
				for (int32 j = 0; j < ScopeDims; ++j)
				{
					DestFloat[j] = SliceFloat[j] * InvNorm;
				}
			}
		}
		else
		{
			FMemory::Memcpy(OutPayload.GetData() + Kept * ScopeRowBytes, SlicePtr, CopyBytes);
			if (Full.quant == Quantization::Int8)
			{
				OutScales.Add(Full.scales[SrcIdx]);
			}
		}

		if (bZeroEnergy)
		{
			++ZeroEnergyExcludedCount;
			if (bCompactZeroEnergy)
			{
				continue; // drop: no index-identity requirement on a sample build
			}
			// Full-view identity: keep the row's slot (zero-filled payload, no scale
			// entry -- never scored, since the caller masks this native index via
			// OutZeroEnergyExcludeBits before querying), but record it as excluded.
			if (Full.quant == Quantization::Int8)
			{
				OutScales.Add(0.0f);
			}
			if (OutZeroEnergyExcludeBits != nullptr)
			{
				(*OutZeroEnergyExcludeBits)[CandidatePos / 32] |= (1u << (CandidatePos % 32));
			}
		}

		OutSourceIndices.Add(SrcIdx);
		++Kept;
	}
	OutPayload.SetNum(Kept * ScopeRowBytes, EAllowShrinking::No);

	OutView = Full;
	OutView.rows = OutPayload.GetData();
	OutView.scales = OutScales.Num() ? OutScales.GetData() : nullptr;
	OutView.count = Kept;
	OutView.dims = ScopeDims;
	OutView.paddedDims = ScopePd;
	OutView.channels = nullptr;
	OutView.channelCount = 0;
	OutView.channelInvNorms = nullptr;
	if (OutZeroEnergyExcludedCount != nullptr)
	{
		*OutZeroEnergyExcludedCount = ZeroEnergyExcludedCount;
	}
	return true;
}

// ---------------------------------------------------------------------------
// The abstraction's own crux. See the header's doc comment on this overload for the
// full disclosure: real delegation on the
// Asset-kind path (bit-identical to the overload above); a real, disclosed rejection for
// a channel-scoped Archive-kind source (not yet supported); and, on the "(whole row)"
// Archive-kind path, the space law enforced for real -- tombstoned rows dropped before
// striding, over the live-row list rather than the published range.
// ---------------------------------------------------------------------------

bool SSuperFAISSBankInspector::BuildAnalysisSample(const FSuperFAISSInspectionSource& Source, int32 SampleLimit,
	TArray<uint8, TAlignedHeapAllocator<16>>& OutPayload, TArray<float>& OutScales,
	superfaiss::BankView& OutView, TArray<int32>& OutSourceIndices, bool bSkipTombstonedRows,
	int32* OutZeroEnergyExcludedCount, TArray<uint32>* OutZeroEnergyExcludeBits) const
{
	using namespace superfaiss;

	if (Source.Kind == FSuperFAISSInspectionSource::EKind::Asset)
	{
		USuperFAISSVectorBank* AssetPtr = Source.Asset.Get();
		if (AssetPtr == nullptr || !AssetPtr->IsValid())
		{
			return false;
		}
		// Finding 6: bCompactZeroEnergy is DERIVED from bSkipTombstonedRows -- both flags
		// select the same axis (sample vs. full-view identity) by construction. Every call
		// site that passes false here (a full-view identity build) needs the row kept at
		// its native index; every call site that leaves the true default (a sample) needs
		// nothing more than the drop this already did.
		return BuildAnalysisSample(*AssetPtr, SampleLimit, OutPayload, OutScales, OutView, OutSourceIndices,
			OutZeroEnergyExcludedCount, /*bCompactZeroEnergy*/ bSkipTombstonedRows, OutZeroEnergyExcludeBits);
	}
	if (Source.Kind != FSuperFAISSInspectionSource::EKind::Archive || !Source.IsValid())
	{
		return false;
	}
	// Real, disclosed rejection (not a silent wrong slice, see the header comment):
	// channel-scoped archive analysis is not yet supported this round.
	if (SelectedProjectionScope.IsValid() && *SelectedProjectionScope != TEXT("(whole row)"))
	{
		return false;
	}

	const BankView Full = Source.GetBankView();
	if (Full.count <= 0)
	{
		return false;
	}

	const int32 ElemBytes = ElementSize(Full.quant);
	const int64 CopyBytes = static_cast<int64>(Full.dims) * ElemBytes;
	const int64 FullRowBytes = RowBytes(Full);

	// The space law's TWO placements — see the header's doc
	// comment on bSkipTombstonedRows. Construction-time discharge (skip=true, the
	// default): the live-only source-index list, ascending, tombstoned rows dropped
	// BEFORE striding. Full-view identity (skip=false): the raw published range
	// unchanged, tombstoned rows included — the caller owns removal via a separate
	// runtime-OR excludeBits aligned to THIS same index space.
	TArray<int32> LiveIndices;
	LiveIndices.Reserve(Full.count);
	if (bSkipTombstonedRows)
	{
		const TArray<uint32> Tombstones = Source.GetTombstoneWords();
		for (int32 i = 0; i < Full.count; ++i)
		{
			const bool bTombstoned = Tombstones.IsValidIndex(i / 32) &&
				(Tombstones[i / 32] & (1u << (i % 32))) != 0;
			if (!bTombstoned)
			{
				LiveIndices.Add(i);
			}
		}
		if (LiveIndices.Num() == 0)
		{
			// Every published row is tombstoned — nothing live to sample (audit F4's
			// live-0 count class), the same "nothing to sample" rejection the caller
			// already surfaces as its own line-item status.
			return false;
		}
	}
	else
	{
		for (int32 i = 0; i < Full.count; ++i)
		{
			LiveIndices.Add(i);
		}
	}

	// The SAME endpoint-inclusive even-sampling arithmetic as the overload above,
	// applied over the LIVE-row list instead of the raw published range.
	const int32 LiveCount = LiveIndices.Num();
	const int32 SampleCount = FMath::Min(LiveCount, FMath::Max(1, SampleLimit));
	OutSourceIndices.Reset();
	OutSourceIndices.Reserve(SampleCount);
	if (SampleCount <= 1 || LiveCount <= 1)
	{
		for (int32 i = 0; i < SampleCount; ++i)
		{
			OutSourceIndices.Add(LiveIndices[i]);
		}
	}
	else
	{
		const int64 Denom = SampleCount - 1;
		for (int32 i = 0; i < SampleCount; ++i)
		{
			const int64 Numer = static_cast<int64>(i) * (LiveCount - 1);
			const int32 LivePos = static_cast<int32>((Numer + Denom - 1) / Denom);
			OutSourceIndices.Add(LiveIndices[LivePos]);
		}
	}

	OutPayload.SetNumZeroed(OutSourceIndices.Num() * static_cast<int64>(Full.paddedDims) * ElemBytes);
	OutScales.Reset();
	for (int32 S = 0; S < OutSourceIndices.Num(); ++S)
	{
		FMemory::Memcpy(OutPayload.GetData() + S * static_cast<int64>(Full.paddedDims) * ElemBytes,
			static_cast<const uint8*>(Full.rows) + OutSourceIndices[S] * FullRowBytes, CopyBytes);
		if (Full.quant == Quantization::Int8)
		{
			OutScales.Add(Full.scales[OutSourceIndices[S]]);
		}
	}

	OutView = Full;
	OutView.rows = OutPayload.GetData();
	OutView.scales = OutScales.Num() ? OutScales.GetData() : nullptr;
	OutView.count = OutSourceIndices.Num();
	// An Archive-kind source rejects a channel scope outright above, so it never reaches
	// a zero-energy Cosine slice to exclude.
	if (OutZeroEnergyExcludedCount != nullptr)
	{
		*OutZeroEnergyExcludedCount = 0;
	}
	if (OutZeroEnergyExcludeBits != nullptr)
	{
		OutZeroEnergyExcludeBits->Reset();
	}
	return true;
}

void SSuperFAISSBankInspector::ComputeStructure()
{
	using namespace superfaiss;
	// Plan section 25.6's addendum on plugin plan section 5.1: the "structure build"
	// named editor-side scope, riding the runtime module's own instrumented query
	// path underneath (BuildAnalysisSample/BuildKnnNeighbors etc.).
	TRACE_CPUPROFILER_EVENT_SCOPE_ON_CHANNEL(TEXT("SuperFAISS.Inspector.StructureBuild"), SuperFAISS);

	StructureClusters.Reset();
	StructureOutlierSampleIndices.Reset();
	StructureComponentIdBySampleIndex.Reset();
	StructureSampleSourceIndices.Reset();
	StructureNeighborSampleIndices.Reset();
	StructureNeighborK = 0;
#if WITH_DEV_AUTOMATION_TESTS
	StructureChunksProcessedForTest = 0;
#endif

	// The source generalization -- an asset OR an open archive, resolved uniformly. Real,
	// mechanical repointing (GetPrimarySource()'s own asset branch is bit-identical to the
	// prior GetSelectedBank() read); the crux gap this leaves unbuilt for an archive lives
	// in BuildAnalysisSample(Source, ...) below, not here.
	const FSuperFAISSInspectionSource Source = GetPrimarySource();
	if (!Source.IsValid())
	{
		StructureStatus = TEXT("no valid bank selected");
		return;
	}

	const USuperFAISSInspectorSettings* Settings = GetDefault<USuperFAISSInspectorSettings>();
	const int32 SampleLimit = Settings->GetClampedSampleLimit();
	const int32 StructureK = Settings->GetClampedStructureK();
	const int32 MinComponentSize = Settings->GetClampedMinComponentSize();

	// Chunked modal slow task ("every bank-wide Inspector pass"): each pipeline STAGE is
	// one chunk boundary, not a row range within one stage —
	// BuildKnnNeighbors/MutualFilter/BuildDuplicateGroups/ConnectedComponents are each a
	// single atomic core call (not row-interruptible mid-call by design, the batch query
	// path's whole point), so cancellation is granular to stages, matching the pipeline
	// graph.h's own ConnectedComponents documents composing.
	enum class EStage : int32 { Sample, Neighbors, Mutual, Duplicates, Components, Count };

	TArray<uint8, TAlignedHeapAllocator<16>> Payload;
	TArray<float> Scales;
	BankView View;
	TArray<int32> SourceIndices;
	Workspace Ws;
	TArray<int32> Neighbors;
	TArray<uint8> MutualFlags;
	TArray<int32> DupGroups, DupScratch;
	TArray<int32> ComponentId, UnionScratch;
	bool bStageFailed = false;
	FString FailStatus;
	int32 ZeroEnergyExcluded = 0;

#if WITH_DEV_AUTOMATION_TESTS
	const auto StagePoll = [this]() {
		return DebugCancelAfterChunks >= 0 &&
			StructureChunksProcessedForTest >= DebugCancelAfterChunks;
	};
#else
	const auto StagePoll = []() { return false; }; // real cancel: SlowTask.ShouldCancel() inside RunChunked
#endif
	const auto RunStage = [&](int32 StageIndex, int32) {
		if (bStageFailed) { return; } // a prior stage already failed; skip the rest
		switch (static_cast<EStage>(StageIndex))
		{
		case EStage::Sample:
			if (!BuildAnalysisSample(Source, SampleLimit, Payload, Scales, View, SourceIndices,
					/*bSkipTombstonedRows*/ true, &ZeroEnergyExcluded))
			{
				bStageFailed = true;
				FailStatus = TEXT("unknown channel scope");
				return;
			}
			if (View.count <= StructureK)
			{
				bStageFailed = true;
				FailStatus = TEXT("sample too small for the configured StructureK");
				return;
			}
			break;
		case EStage::Neighbors:
			Neighbors.SetNumUninitialized(View.count * StructureK);
			if (BuildKnnNeighbors(View, StructureK, /*excludeSelf*/ true, Neighbors.GetData(), Ws) !=
				Status::Ok)
			{
				bStageFailed = true;
				FailStatus = TEXT("structure pass failed");
				return;
			}
			break;
		case EStage::Mutual:
			MutualFlags.SetNumUninitialized(View.count * StructureK);
			MutualFilter(View.count, StructureK, Neighbors.GetData(), MutualFlags.GetData());
			break;
		case EStage::Duplicates:
			DupGroups.SetNumUninitialized(View.count);
			DupScratch.SetNumUninitialized(View.count);
			BuildDuplicateGroups(View, DupGroups.GetData(), DupScratch.GetData());
			break;
		case EStage::Components:
			ComponentId.SetNumUninitialized(View.count);
			UnionScratch.SetNumUninitialized(View.count);
			ConnectedComponents(View.count, StructureK, Neighbors.GetData(), MutualFlags.GetData(),
				DupGroups.GetData(), ComponentId.GetData(), UnionScratch.GetData());
			break;
		default:
			break;
		}
#if WITH_DEV_AUTOMATION_TESTS
		StructureChunksProcessedForTest = StageIndex + 1;
#endif
	};

	const SuperFAISSInspectorSlowTask::FResult ChunkResult = SuperFAISSInspectorSlowTask::RunChunked(
		FText::FromString(TEXT("Computing structure...")), static_cast<int32>(EStage::Count), 1,
		RunStage, StagePoll);

	if (ChunkResult.bCancelled)
	{
		StructureStatus = TEXT("cancelled");
		return;
	}
	if (bStageFailed)
	{
		StructureStatus = FailStatus;
		return;
	}

	// Group sample positions by canonical component id (already the smallest
	// member index — a deterministic, order-independent grouping key).
	TMap<int32, TArray<int32>> Members;
	for (int32 i = 0; i < View.count; ++i)
	{
		Members.FindOrAdd(ComponentId[i]).Add(i);
	}
	StructureComponentIdBySampleIndex.Init(-1, View.count);

	TArray<int32> Ids;
	Members.GetKeys(Ids);
	Ids.Sort();
	for (const int32 Id : Ids)
	{
		const TArray<int32>& MemberList = Members[Id];
		if (MemberList.Num() < MinComponentSize)
		{
			StructureOutlierSampleIndices.Append(MemberList);
			// StructureComponentIdBySampleIndex[*] stays -1 for these (Init above).
		}
		else
		{
			FSuperFAISSStructureCluster Cluster;
			Cluster.ComponentId = Id;
			Cluster.MemberSampleIndices = MemberList;
			StructureClusters.Add(MoveTemp(Cluster));
			for (const int32 SampleIdx : MemberList)
			{
				StructureComponentIdBySampleIndex[SampleIdx] = Id;
			}
		}
	}

	// Persisted past this function's return (section 25.5 "members by Id" + "its k
	// nearest"): the UI resolves a member row's real bank id and neighbor set without
	// re-running the pipeline.
	StructureSampleSourceIndices = SourceIndices;
	StructureNeighborSampleIndices = Neighbors;
	StructureNeighborK = StructureK;

	// GetLiveCount(), not GetCount(): BuildAnalysisSample is called with bSkipTombstonedRows
	// == true above, so tombstoned rows are dropped before striding and can never be
	// sampled -- the published count would sit in the denominator despite being
	// unreachable, understating the sampled fraction on a pruned archive.
	StructureStatus = FString::Printf(TEXT("%d of %d rows (sampled)"), View.count, Source.GetLiveCount());
	if (ZeroEnergyExcluded > 0)
	{
		// A zero-norm Cosine row has no direction to compare and is excluded from the
		// sample, never silently included with an undefined direction.
		StructureStatus += FString::Printf(
			TEXT(", %d excluded (zero energy in channel)"), ZeroEnergyExcluded);
	}
}

void SSuperFAISSBankInspector::ProbeNovelty(const FString& Text)
{
	using namespace superfaiss;
	// Plan section 25.6's addendum: the "novelty calibration" named editor-side scope.
	TRACE_CPUPROFILER_EVENT_SCOPE_ON_CHANNEL(TEXT("SuperFAISS.Inspector.NoveltyCalibration"), SuperFAISS);

	NoveltyEvidenceLines.Reset();
	NoveltyResult = FSuperFAISSNoveltyResult();

	USuperFAISSVectorBank* Bank = GetSelectedBank();
	USuperFAISSSubsystem* Subsystem = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
	if (Bank == nullptr || !Bank->IsValid() || Subsystem == nullptr)
	{
		NoveltyStatus = TEXT("no valid bank selected");
		return;
	}

	// Row lookup: identical parsing to RunQuery (row id or #index).
	const FString Trimmed = Text.TrimStartAndEnd();
	int32 Row = INDEX_NONE;
	if (Trimmed.StartsWith(TEXT("#")))
	{
		Row = FCString::Atoi(*Trimmed.Mid(1));
	}
	else
	{
		Row = Bank->GetIndexForId(FName(*Trimmed));
	}
	TArray<float> RawQuery;
	if (Row < 0 || Row >= Bank->Count || !Subsystem->MakeCentroidQuery(Bank, {Row}, RawQuery))
	{
		NoveltyStatus = FString::Printf(TEXT("row not found: %s"), *Trimmed);
		return;
	}

	NoveltyResult.bValid = true;
	NoveltyStatus.Reset();

	if (Bank->Metric == ESuperFAISSBankMetric::Dot)
	{
		NoveltyResult.Verdict = ESuperFAISSNoveltyVerdict::Unavailable;
		NoveltyResult.UnavailableStatus = DotUnavailableStatus();
		return;
	}

	const BankView Full = Bank->GetBankView();

	// The limb-1 exact-distance probe is decoded via the SAME primitive the kernels
	// themselves use (DequantizeRowAsQuery, inspector_common.h), not
	// MakeCentroidQuery's double-accumulate-then-narrow path: for a probed row that
	// is itself a bank row, NoveltyProbeDistance's int8 dispatch REQUANTIZES the
	// float probe, and MakeCentroidQuery's extra double round-trip can land one ULP
	// off the row's own stored bytes on re-quantization — which breaks the exact-
	// zero identity test on a true duplicate (observed: -2.22e-16, not 0.0f, on the
	// dequant-identical/different-scale int8 L2 fixture). DequantizeRowAsQuery's
	// direct float(byte)*float(scale) decode round-trips exactly.
	TArray<float> PaddedProbe;
	PaddedProbe.SetNumUninitialized(Full.paddedDims);
	DequantizeRowAsQuery(Full, Row, PaddedProbe.GetData());

	int32 Channel = INDEX_NONE;
	if (SelectedProjectionScope.IsValid() && *SelectedProjectionScope != TEXT("(whole row)"))
	{
		Channel = Bank->GetChannelIndex(FName(**SelectedProjectionScope));
		if (Channel == INDEX_NONE)
		{
			NoveltyStatus = TEXT("unknown channel scope");
			NoveltyResult = FSuperFAISSNoveltyResult();
			return;
		}
	}

	// Self-exclusion bitmask over the FULL bank (V32-G7) — reused for the limb-1
	// nearest-row search and the evidence list.
	TArray<uint32> ExcludeSelf;
	ExcludeSelf.SetNumZeroed(FMath::DivideAndRoundUp(Bank->Count, 32));
	ExcludeSelf[Row / 32] |= (1u << (Row % 32));

	// Limb 1 (identity): the nearest OTHER row's own exact metric
	// distance via NoveltyProbeDistance — exactly 0 -> Duplicate. The probe stays
	// unnormalized here; NoveltyProbeDistance's Cosine leg is magnitude-invariant by
	// construction (it divides by sqrt(aSq*bSq)), unlike limb 2 below.
	{
		FSuperFAISSQueryArgs NearestArgs;
		NearestArgs.K = 1;
		NearestArgs.ExcludeBits = ExcludeSelf;
		if (Channel != INDEX_NONE)
		{
			NearestArgs.Channels.Add({Bank->ChannelNames[Channel], 1.0f});
		}
		TArray<FSuperFAISSHit> NearestHit;
		if (Subsystem->QuerySync(Bank, RawQuery, NearestArgs, NearestHit) && NearestHit.Num() > 0)
		{
			float ExactDistance = 0.0f;
			Workspace NearestWs;
			// Limb 1 chooses no numeric threshold of its own -- the test is the METRIC'S OWN
			// exact zero, read from the metric, never a single epsilon applied uniformly.
			// Cosine's int8 parallel-code arithmetic (and a byte-identical float32
			// duplicate) achieves true bit-exact 0 for a real duplicate, so Cosine tests
			// `== 0.0f`; a near-duplicate that is merely close must fall through to limb 2,
			// never read as Duplicate. L2's expanded form (a^2+b^2-2ab) carries an inherent,
			// disclosed double-precision cancellation residue even on an exact duplicate --
			// the core suite's own oracle (TestM2NoveltyProbeDistance) reserves the 1e-8f
			// epsilon for L2 only.
			if (NoveltyProbeDistance(Full, PaddedProbe.GetData(), NearestHit[0].Index, Channel,
					&ExactDistance, NearestWs) == Status::Ok)
			{
				const bool bDuplicate = Full.metric == Metric::Cosine
					? ExactDistance == 0.0f
					: FMath::Abs(ExactDistance) < 1e-8f;
				if (bDuplicate)
				{
					NoveltyResult.Verdict = ESuperFAISSNoveltyVerdict::Duplicate;
				}
			}
		}
	}

	if (NoveltyResult.Verdict != ESuperFAISSNoveltyVerdict::Duplicate)
	{
		// Limb 2 (statistical rank, research 4.4): the probe's own k-th-NN RankDistance
		// against the bank's calibrated baseline, scored as a CDF rank vs lambda.
		const USuperFAISSInspectorSettings* Settings = GetDefault<USuperFAISSInspectorSettings>();
		const int32 SampleLimit = Settings->GetClampedSampleLimit();
		const int32 NoveltyK = Settings->GetClampedNoveltyK();
		const float Lambda = Settings->GetClampedNoveltyLambda();

		TArray<uint8, TAlignedHeapAllocator<16>> Payload;
		TArray<float> Scales;
		BankView SampledView;
		TArray<int32> SampledSourceIndices;
		// The disclosure requirement: a sample build (bCompactZeroEnergy stays at its true
		// default), so surfaced via NoveltyResult.ZeroEnergyExcludedCount below rather than
		// a status string -- SampledCount/TotalCount already use the struct, not
		// NoveltyStatus, for exactly this kind of disclosure.
		int32 ZeroEnergyExcludedNovelty = 0;
		if (!BuildAnalysisSample(*Bank, SampleLimit, Payload, Scales, SampledView, SampledSourceIndices,
				&ZeroEnergyExcludedNovelty))
		{
			NoveltyStatus = TEXT("unknown channel scope");
			NoveltyResult = FSuperFAISSNoveltyResult();
			return;
		}
		if (SampledView.count <= NoveltyK)
		{
			NoveltyStatus = TEXT("sample too small for the configured NoveltyK");
			NoveltyResult = FSuperFAISSNoveltyResult();
			return;
		}

		// Recalibrate whenever the cached baseline's own K/SampleLimit no longer match the
		// CURRENT settings, not only on an explicit invalidation event -- a settings edit is
		// not a widget event this class can observe, so staleness must be self-detected
		// here, structurally, every probe, rather than relying on someone remembering to
		// fire an invalidation trigger that does not exist for this case.
		if (!bNoveltyBaselineCalibrated || NoveltyBaselineK != NoveltyK ||
			NoveltyBaselineSampleLimit != SampleLimit)
		{
			// Chunked modal slow task: the baseline
			// calibration is the "bank-wide pass" leg of a probe (the per-probe limb-1/
			// limb-2 work above and below is fast and does not warrant a dialog). One
			// chunk — CalibrateNoveltyBaseline is a single atomic core call, same
			// stage-not-row-range reasoning as ComputeStructure's pipeline.
			bool bCalibrateFailed = false;
			int32 BaselineCount = 0;
#if WITH_DEV_AUTOMATION_TESTS
			NoveltyBaselineChunksProcessedForTest = 0;
			const auto BaselinePoll = [this]() {
				return DebugCancelAfterChunks >= 0 &&
					NoveltyBaselineChunksProcessedForTest >= DebugCancelAfterChunks;
			};
#else
			const auto BaselinePoll = []() { return false; };
#endif
			NoveltyBaselineSortedDistances.SetNumUninitialized(SampledView.count);
			const SuperFAISSInspectorSlowTask::FResult CalibResult = SuperFAISSInspectorSlowTask::RunChunked(
				FText::FromString(TEXT("Calibrating novelty baseline...")), 1, 1,
				[&](int32, int32) {
					Workspace BaselineWs;
					bCalibrateFailed = CalibrateNoveltyBaseline(SampledView, NoveltyK, SampleLimit,
						NoveltyBaselineSortedDistances.GetData(), &BaselineCount, BaselineWs) != Status::Ok;
#if WITH_DEV_AUTOMATION_TESTS
					NoveltyBaselineChunksProcessedForTest = 1;
#endif
				},
				BaselinePoll);

			if (CalibResult.bCancelled)
			{
				NoveltyStatus = TEXT("cancelled");
				NoveltyResult = FSuperFAISSNoveltyResult();
				NoveltyBaselineSortedDistances.Reset();
				return;
			}
			if (bCalibrateFailed)
			{
				NoveltyStatus = TEXT("baseline calibration failed");
				NoveltyResult = FSuperFAISSNoveltyResult();
				NoveltyBaselineSortedDistances.Reset();
				bNoveltyBaselineCalibrated = false;
				return;
			}
			NoveltyBaselineSortedDistances.SetNum(BaselineCount);
			bNoveltyBaselineCalibrated = true;
			NoveltyBaselineK = NoveltyK;
			NoveltyBaselineSampleLimit = SampleLimit;
		}

		// KthNeighborDistance has no channel parameter of its own (unlike
		// NoveltyProbeDistance, which slices internally) — the caller presents an
		// already-scoped query matching SampledView's own dims/paddedDims.
		TArray<float> ScopedQuery;
		ScopedQuery.SetNumZeroed(SampledView.paddedDims);
		if (Channel == INDEX_NONE)
		{
			FMemory::Memcpy(ScopedQuery.GetData(), RawQuery.GetData(),
				SampledView.dims * sizeof(float));
		}
		else
		{
			FMemory::Memcpy(ScopedQuery.GetData(),
				RawQuery.GetData() + Bank->ChannelOffsets[Channel], SampledView.dims * sizeof(float));
		}
		if (Bank->Metric == ESuperFAISSBankMetric::Cosine)
		{
			// Normalize the probe to unit L2 norm at this
			// call site, never inside KthNeighborDistance itself — that function rides
			// Query()'s plain dot product over pre-normalized stored rows, magnitude-
			// invariant only when the probe is also unit-norm.
			double SumSq = 0.0;
			for (int32 d = 0; d < SampledView.dims; ++d)
			{
				SumSq += static_cast<double>(ScopedQuery[d]) * ScopedQuery[d];
			}
			const double Norm = FMath::Sqrt(SumSq);
			if (Norm > 0.0)
			{
				const float InvNorm = static_cast<float>(1.0 / Norm);
				for (int32 d = 0; d < SampledView.dims; ++d)
				{
					ScopedQuery[d] *= InvNorm;
				}
			}
		}

		// Exclude the probed row from its own baseline candidate set, if it happens
		// to be one of the sampled rows (it may not be, once Count > SampleLimit).
		TArray<uint32> SampleExclude;
		SampleExclude.SetNumZeroed(FMath::DivideAndRoundUp(SampledView.count, 32));
		for (int32 S = 0; S < SampledSourceIndices.Num(); ++S)
		{
			if (SampledSourceIndices[S] == Row)
			{
				SampleExclude[S / 32] |= (1u << (S % 32));
				break;
			}
		}

		float RankDist = 0.0f;
		Workspace ProbeWs;
		if (KthNeighborDistance(SampledView, ScopedQuery.GetData(), NoveltyK,
				SampleExclude.GetData(), &RankDist, ProbeWs) != Status::Ok)
		{
			NoveltyStatus = TEXT("novelty probe failed");
			NoveltyResult = FSuperFAISSNoveltyResult();
			return;
		}
		float Score = 0.0f;
		NoveltyScore(NoveltyBaselineSortedDistances.GetData(), NoveltyBaselineSortedDistances.Num(),
			RankDist, &Score);

		NoveltyResult.Score = Score;
		NoveltyResult.Verdict =
			Score >= Lambda ? ESuperFAISSNoveltyVerdict::Novel : ESuperFAISSNoveltyVerdict::Familiar;
		NoveltyResult.bLowConfidence = SampledView.count < NoveltyLowConfidenceFloor;
		NoveltyResult.SampledCount = SampledView.count;
		NoveltyResult.TotalCount = Bank->Count;
		NoveltyResult.ZeroEnergyExcludedCount = ZeroEnergyExcludedNovelty;
	}

	// Evidence list (V32-G6): the k nearest FULL-BANK rows, self-excluded — evidence,
	// never the verdict's basis; recomputed per probe, never cached (re-gate F7).
	{
		const USuperFAISSInspectorSettings* Settings = GetDefault<USuperFAISSInspectorSettings>();
		FSuperFAISSQueryArgs EvidenceArgs;
		EvidenceArgs.K = Settings->GetClampedNoveltyK();
		EvidenceArgs.ExcludeBits = ExcludeSelf;
		if (Channel != INDEX_NONE)
		{
			EvidenceArgs.Channels.Add({Bank->ChannelNames[Channel], 1.0f});
		}
		TArray<FSuperFAISSHit> EvidenceHits;
		if (Subsystem->QuerySync(Bank, RawQuery, EvidenceArgs, EvidenceHits))
		{
			for (const FSuperFAISSHit& Hit : EvidenceHits)
			{
				NoveltyEvidenceLines.Add(MakeShared<FString>(FString::Printf(
					TEXT("%s  score %.4f   margin %.4f"),
					Hit.Id.IsNone() ? *FString::Printf(TEXT("#%d"), Hit.Index) : *Hit.Id.ToString(),
					Hit.Score, Hit.Margin)));
			}
		}
	}
}

// ---------------------------------------------------------------------------
// V3.2 plan section 25.5 — View C (Correspondence), slot 4, CLOSED GREEN. The
// second-bank compatibility check (CheckSecondBankCompatible, above) is plain field
// comparisons already available on USuperFAISSVectorBank (the same reasoning already
// applied to RefreshSecondBankList/GetSelectedSecondBank). The compute wires
// BuildAnalysisSample's A-side sample plus full B/A views (BuildAnalysisSample again,
// called with each bank's own live Count as the limit — ceiling-division sampling is
// the identity permutation when SampleCount == Full.count, so no second
// view-construction path was needed) through matching.h's MutualNearestMatches (closed
// green core-side, reviewed), inside ONE chunk boundary of the
// already-real SuperFAISSInspectorSlowTask::RunChunked. State (matched/ambiguous/
// unmatched) is caller-composed from each pair's raw CslsMargin against
// Settings->CslsMarginThreshold, mirroring novelty.h's NoveltyScore-vs-lambda
// precedent — matching.h defines no verdict of its own.
//
// Status-line contract (unmatched counts for BOTH sides shown in the status line; the
// coverage line reads, VERBATIM, "N of M A-rows
// checked"): on a successful compute the status is `"<checked> of <A.Count> A-rows
// checked, <unmatchedA> unmatched (A), <unmatchedB> unmatched (B)<mixed-quant
// suffix>"`, where `<mixed-quant suffix>` is `", mixed quantization"` when the primary
// and second bank's Quantization differ ("a mixed pair is disclosed in the
// status line") and empty otherwise. Unmatched-B is counted as distinct B indices that
// never appear as a matched partner (a stated forced reading — the plan does not
// itself define the B-side term).
// ---------------------------------------------------------------------------

void SSuperFAISSBankInspector::ComputeCorrespondence()
{
	using namespace superfaiss;
	// Plan section 25.6's addendum: the "correspondence match" named editor-side scope.
	TRACE_CPUPROFILER_EVENT_SCOPE_ON_CHANNEL(TEXT("SuperFAISS.Inspector.CorrespondenceMatch"), SuperFAISS);

	MatchPairResults.Reset();
	MatchPairDisplayLines.Reset();
	CorrespondenceStatus.Reset();
#if WITH_DEV_AUTOMATION_TESTS
	CorrespondenceChunksProcessedForTest = 0;
#endif

	// The source generalization, extended to BOTH slots
	// ("archive-vs-baked correspondence... exercised in slot A and in slot B
	// separately"). Real, mechanical repointing.
	const FSuperFAISSInspectionSource PrimarySource = GetPrimarySource();
	if (!PrimarySource.IsValid())
	{
		CorrespondenceStatus = TEXT("no valid bank selected");
		return;
	}

	const FSuperFAISSInspectionSource SecondSource = GetSecondSource();
	FString RejectReason;
	if (!CheckSecondBankCompatible(PrimarySource, SecondSource, RejectReason))
	{
		// dim-5 audit N-3, the late-rejection UI contract: MatchPairResults was already
		// Reset() above, unconditionally, before this check ran — a rejection never
		// leaves a stale pair list from an earlier valid pair rendering beside it.
		CorrespondenceStatus = RejectReason;
		return;
	}

	const USuperFAISSInspectorSettings* Settings = GetDefault<USuperFAISSInspectorSettings>();
	const int32 SampleLimit = Settings->GetClampedSampleLimit();
	const int32 ClampedMatchK = Settings->GetClampedMatchK();

	TArray<uint8, TAlignedHeapAllocator<16>> PayloadA, PayloadFullB, PayloadFullA;
	TArray<float> ScalesA, ScalesFullB, ScalesFullA;
	BankView ViewA, ViewFullB, ViewFullA;
	TArray<int32> SourceIndicesA, SourceIndicesFullB, SourceIndicesFullA;
	// The space law's runtime-OR leg (section 25.3): source-space tombstone words for the
	// full B / full A views, populated inside the chunk lambda below, kept alive through
	// the MutualNearestMatches call outside it.
	TArray<uint32> ExcludeBitsFullB, ExcludeBitsFullA;
	// Finding 6 (regression on Finding 1): the SAME shape, for a channel-scoped Cosine
	// zero-energy row on the full-view identity build -- OR'd into ExcludeBitsFullB/A
	// below, alongside the tombstone words, so a zero-energy row is never a candidate
	// MutualNearestMatches can match, exactly like a tombstoned one.
	TArray<uint32> ZeroEnergyBitsFullB, ZeroEnergyBitsFullA;
	int32 ZeroEnergyExcludedFullB = 0, ZeroEnergyExcludedFullA = 0;
	Workspace Ws;
	TArray<MatchPair> RawPairs;
	int32 RawPairCount = 0;
	bool bFailed = false;
	FString FailStatus;

#if WITH_DEV_AUTOMATION_TESTS
	const auto StagePoll = [this]() {
		return DebugCancelAfterChunks >= 0 &&
			CorrespondenceChunksProcessedForTest >= DebugCancelAfterChunks;
	};
#else
	const auto StagePoll = []() { return false; }; // real cancel: SlowTask.ShouldCancel() inside RunChunked
#endif

	const SuperFAISSInspectorSlowTask::FResult ChunkResult = SuperFAISSInspectorSlowTask::RunChunked(
		FText::FromString(TEXT("Computing correspondence...")), 1, 1,
		[&](int32, int32) {
			// The A-side sample: the SAME construction View A/B's baseline uses,
			// honoring the shared analysis scope. This is where the space law's
			// construction-time-discharge leg lives (BuildAnalysisSample(Source, ...));
			// an archive-sourced A carries the slot 4b crux gap disclosed there.
			if (!BuildAnalysisSample(PrimarySource, SampleLimit, PayloadA, ScalesA, ViewA, SourceIndicesA))
			{
				bFailed = true;
				FailStatus = TEXT("unknown channel scope");
				return;
			}
			// Full-bank views of B (the forward-pass target) and A (back-verification
			// needs the FULL primary bank, not the sample) — the SAME channel-scope
			// slicing BuildAnalysisSample already does for a sample, called with each
			// source's own PUBLISHED count so the "sample" is the identity: every
			// published row (tombstoned or not — see ExcludeBitsA/B below, the space
			// law's runtime-OR leg, which is what actually removes a tombstoned row from
			// matching consideration on this identity-mapped full view, real and wired
			// this round regardless of the A-side sample gap above).
			// bSkipTombstonedRows=false: these are the space law's OTHER placement (full-
			// view identity, not construction-time discharge) — a tombstoned row must
			// stay IN the view, at its native index, so ExcludeBitsFullB/A (computed
			// below, in that same native space) land on the right row.
			if (!BuildAnalysisSample(SecondSource, SecondSource.GetCount(), PayloadFullB, ScalesFullB,
					ViewFullB, SourceIndicesFullB, /*bSkipTombstonedRows*/ false,
					&ZeroEnergyExcludedFullB, &ZeroEnergyBitsFullB) ||
				!BuildAnalysisSample(PrimarySource, PrimarySource.GetCount(), PayloadFullA, ScalesFullA,
					ViewFullA, SourceIndicesFullA, /*bSkipTombstonedRows*/ false,
					&ZeroEnergyExcludedFullA, &ZeroEnergyBitsFullA))
			{
				bFailed = true;
				FailStatus = TEXT("unknown channel scope");
				return;
			}

			// The space law's runtime-OR leg (section 25.3): "the full-view evidence
			// query carries the OR, in source space" — the SAME rule applies to
			// Correspondence's own full-view scoring passes (the section 25.3 scrub:
			// "Correspondence's scoring passes... run against the full live views and
			// carry the runtime OR in each view's own source space"). Always empty for
			// an asset source (GetTombstoneWords() is empty there — a no-op OR), real
			// tombstone words for an archive. Declared outside the lambda so the data
			// pointers MutualNearestMatches reads stay alive through the call.
			ExcludeBitsFullB = SecondSource.GetTombstoneWords();
			ExcludeBitsFullA = PrimarySource.GetTombstoneWords();
			// Finding 6: the zero-energy leg of the SAME OR, in the SAME native index
			// space -- a channel-scoped Cosine zero-energy row is excluded from matching
			// exactly like a tombstoned row, composed rather than double-counted (a row
			// that happens to be both tombstoned and zero-energy sets one bit, not two).
			MergeExcludeBits(ExcludeBitsFullB, ZeroEnergyBitsFullB);
			MergeExcludeBits(ExcludeBitsFullA, ZeroEnergyBitsFullA);

			RawPairs.SetNumUninitialized(ViewA.count);
			const Status Result = MutualNearestMatches(ViewA, SourceIndicesA.GetData(),
				ViewFullB, ExcludeBitsFullB.Num() ? ExcludeBitsFullB.GetData() : nullptr,
				ViewFullA, ExcludeBitsFullA.Num() ? ExcludeBitsFullA.GetData() : nullptr,
				ClampedMatchK, RawPairs.GetData(), &RawPairCount, Ws);
			if (Result != Status::Ok)
			{
				bFailed = true;
				FailStatus = TEXT("correspondence pass failed");
				return;
			}
#if WITH_DEV_AUTOMATION_TESTS
			CorrespondenceChunksProcessedForTest = 1;
#endif
		},
		StagePoll);

	if (ChunkResult.bCancelled)
	{
		// "No partial cache commit": no pair was ever written to
		// MatchPairResults itself (only to the local RawPairs scratch), so it stays at
		// its top-of-function Reset() (empty).
		MatchPairResults.Reset();
		CorrespondenceStatus = TEXT("cancelled");
		return;
	}
	if (bFailed)
	{
		CorrespondenceStatus = FailStatus;
		return;
	}

	// State (matched/ambiguous/unmatched) is CALLER-composed from each pair's raw
	// CslsMargin against Settings->CslsMarginThreshold (matching.h's own contract: no
	// verdict entry exists on MatchPair itself, mirroring novelty.h's NoveltyScore).
	MatchPairResults.Reserve(RawPairCount);
	MatchPairDisplayLines.Reserve(RawPairCount);
	int32 UnmatchedA = 0;
	TSet<int32> MatchedBIndices;
	for (int32 i = 0; i < RawPairCount; ++i)
	{
		const MatchPair& Raw = RawPairs[i];
		FSuperFAISSMatchPairResult Result;
		Result.SourceIndexA = Raw.sourceIndexA;
		Result.SourceIndexB = Raw.sourceIndexB;
		Result.CslsMargin = Raw.cslsMargin;
		if (Raw.sourceIndexB < 0)
		{
			Result.State = ESuperFAISSMatchState::Unmatched;
			++UnmatchedA;
			MatchPairDisplayLines.Add(MakeShared<FString>(
				FString::Printf(TEXT("#%d -> unmatched"), Result.SourceIndexA)));
		}
		else
		{
			Result.State = Raw.cslsMargin >= Settings->CslsMarginThreshold
				? ESuperFAISSMatchState::Matched : ESuperFAISSMatchState::Ambiguous;
			MatchedBIndices.Add(Result.SourceIndexB);
			MatchPairDisplayLines.Add(MakeShared<FString>(FString::Printf(
				TEXT("#%d -> #%d  margin %.4f  (%s)"), Result.SourceIndexA, Result.SourceIndexB,
				Result.CslsMargin,
				Result.State == ESuperFAISSMatchState::Matched ? TEXT("matched") : TEXT("ambiguous"))));
		}
		MatchPairResults.Add(Result);
	}

	// "B rows never appearing as any matched partner" (the stated forced reading -- the
	// status-line contract names unmatched counts for both sides without defining the
	// B-side term): the denominator here and below is the MATCHABLE population, never the
	// published count — a row excluded by ExcludeBitsFullB/A (tombstoned OR zero-energy in
	// a channel-scoped Cosine bank) is never a candidate MutualNearestMatches can match, so
	// counting it as "unmatched"/"checked" overstates the total. CountExcludedBits reads
	// the SAME merged bit array MutualNearestMatches itself was called against above, so
	// the denominator and the actual exclusion are the same fact, never two numbers that
	// could drift.
	const int32 MatchableB = SecondSource.GetCount() - CountExcludedBits(ExcludeBitsFullB);
	const int32 MatchableA = PrimarySource.GetCount() - CountExcludedBits(ExcludeBitsFullA);
	const int32 UnmatchedB = MatchableB - MatchedBIndices.Num();
	const bool bMixedQuantization = PrimarySource.GetQuantization() != SecondSource.GetQuantization();
	CorrespondenceStatus = FString::Printf(
		TEXT("%d of %d A-rows checked, %d unmatched (A), %d unmatched (B)%s"),
		RawPairCount, MatchableA, UnmatchedA, UnmatchedB,
		bMixedQuantization ? TEXT(", mixed quantization") : TEXT(""));
	// The disclosure requirement, extended to Correspondence: a
	// zero-energy exclusion on either side is surfaced, never silently folded into a
	// smaller-than-expected sample the way StructureStatus already discloses it.
	if (ZeroEnergyExcludedFullA > 0)
	{
		CorrespondenceStatus += FString::Printf(
			TEXT(", %d excluded (zero energy in channel, A)"), ZeroEnergyExcludedFullA);
	}
	if (ZeroEnergyExcludedFullB > 0)
	{
		CorrespondenceStatus += FString::Printf(
			TEXT(", %d excluded (zero energy in channel, B)"), ZeroEnergyExcludedFullB);
	}
}

const TCHAR* SSuperFAISSBankInspector::StructureDisclosureCopy()
{
	// Section 25.5 View A, verbatim (E-D3-1's designer-answered determinism-tier
	// disclosure).
	return TEXT("Deterministic on this device (fixed sample, fixed order). Layouts ")
		TEXT("and cluster ids may differ across machines — no cross-device claim.");
}

const TCHAR* SSuperFAISSBankInspector::DotUnavailableStatus()
{
	// Section 25.5 View B, verbatim.
	return TEXT("novelty verdict unavailable on Dot banks (dot product is not a dissimilarity)");
}

FString SSuperFAISSBankInspector::BuildNoveltyVerdictText() const
{
	if (!NoveltyResult.bValid)
	{
		return NoveltyStatus;
	}
	if (NoveltyResult.Verdict == ESuperFAISSNoveltyVerdict::Unavailable)
	{
		return NoveltyResult.UnavailableStatus;
	}
	// `duplicate` is decided entirely by limb 1 (the metric's own exact-zero distance,
	// section 25.4); ProbeNovelty's `if (Verdict != Duplicate)` guard never runs limb 2
	// for this verdict, so Score/SampledCount/TotalCount stay at their zeroed defaults.
	// Rendering them here would print a CDF statistic this verdict never consulted.
	if (NoveltyResult.Verdict == ESuperFAISSNoveltyVerdict::Duplicate)
	{
		return TEXT("duplicate");
	}
	const TCHAR* VerdictText = TEXT("?");
	switch (NoveltyResult.Verdict)
	{
	case ESuperFAISSNoveltyVerdict::Novel: VerdictText = TEXT("novel (mint)"); break;
	case ESuperFAISSNoveltyVerdict::Familiar: VerdictText = TEXT("familiar"); break;
	default: VerdictText = TEXT("(not computed)"); break;
	}
	FString Text = FString::Printf(
		TEXT("%s — novelty %.4f vs %d of %d sampled rows%s"), VerdictText,
		NoveltyResult.Score, NoveltyResult.SampledCount, NoveltyResult.TotalCount,
		NoveltyResult.bLowConfidence ? TEXT(" (low confidence)") : TEXT(""));
	if (NoveltyResult.ZeroEnergyExcludedCount > 0)
	{
		// Plan section 25.3's exclusion disclosure, extended to Novelty (Structure and
		// Correspondence already disclose it in their own status lines): the baseline
		// sample dropped this many rows for zero energy in the scoped channel, so
		// SampledCount above is smaller than it would otherwise be for exactly this
		// reason.
		Text += FString::Printf(
			TEXT(", %d excluded (zero energy in channel)"), NoveltyResult.ZeroEnergyExcludedCount);
	}
	return Text;
}

#if WITH_DEV_AUTOMATION_TESTS
void SSuperFAISSBankInspector::SetBankForTest(USuperFAISSVectorBank* Bank)
{
	BankNames.Reset();
	BankAssets.Reset();
	if (Bank != nullptr)
	{
		BankNames.Add(MakeShared<FString>(Bank->GetName()));
		BankAssets.Add(Bank);
		SelectedBankName = BankNames[0];
	}
	else
	{
		SelectedBankName.Reset();
	}
	OnBankSelected();
}

void SSuperFAISSBankInspector::SetSecondBankForTest(USuperFAISSVectorBank* Bank)
{
	SecondBankNames.Reset();
	SecondBankAssets.Reset();
	if (Bank != nullptr)
	{
		SecondBankNames.Add(MakeShared<FString>(Bank->GetName()));
		SecondBankAssets.Add(Bank);
		SelectedSecondBankName = SecondBankNames[0];
	}
	else
	{
		SelectedSecondBankName.Reset();
	}
	OnSecondBankSelected();
}

void SSuperFAISSBankInspector::SetAnalysisScopeForTest(const FString& ScopeName)
{
	for (const TSharedPtr<FString>& Scope : ProjectionScopes)
	{
		if (Scope.IsValid() && *Scope == ScopeName)
		{
			SelectedProjectionScope = Scope;
			ProjectedPoints.Reset();
			ProjectionStatus.Reset();
			InvalidateAnalysisCaches();
			return;
		}
	}
}
#endif // WITH_DEV_AUTOMATION_TESTS
