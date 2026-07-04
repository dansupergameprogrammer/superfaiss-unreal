#include "SSuperFAISSBankInspector.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "SuperFAISSSubsystem.h"
#include "SuperFAISSVectorBank.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Text/STextBlock.h"

#include "superfaiss/superfaiss.h"

namespace
{
	// Scatter pane: paints the projected points; leaf widget, editor-tier.
	class SSuperFAISSScatter : public SLeafWidget
	{
	public:
		SLATE_BEGIN_ARGS(SSuperFAISSScatter) {}
			SLATE_ARGUMENT(const TArray<FVector2f>*, Points)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs) { Points = InArgs._Points; }

		virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& Geometry,
			const FSlateRect& CullingRect, FSlateWindowElementList& OutDrawElements,
			int32 LayerId, const FWidgetStyle& InWidgetStyle,
			bool bParentEnabled) const override
		{
			const FVector2D Size = Geometry.GetLocalSize();
			if (Points != nullptr)
			{
				const FSlateBrush* Brush = FCoreStyle::Get().GetBrush("GenericWhiteBox");
				for (const FVector2f& P : *Points)
				{
					const FVector2D Pos(P.X * (Size.X - 4.0), (1.0 - P.Y) * (Size.Y - 4.0));
					FSlateDrawElement::MakeBox(OutDrawElements, LayerId,
						Geometry.ToPaintGeometry(FVector2D(3.0, 3.0),
							FSlateLayoutTransform(Pos)),
						Brush, ESlateDrawEffect::None,
						FLinearColor(0.35f, 0.75f, 1.0f, 0.85f));
				}
			}
			return LayerId + 1;
		}

		virtual FVector2D ComputeDesiredSize(float) const override
		{
			return FVector2D(420.0, 420.0);
		}

	private:
		const TArray<FVector2f>* Points = nullptr;
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

			// Right: projection visualizer.
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
					// Channel scope for the projection (channel banks only).
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
				+ SVerticalBox::Slot().FillHeight(1.0f)
				[
					SNew(SBorder).Padding(2.0f)
					[
						SNew(SSuperFAISSScatter).Points(&ProjectedPoints)
					]
				]
			]
		]
	];

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

void SSuperFAISSBankInspector::OnBankSelected()
{
	ProjectedPoints.Reset();
	ProjectionStatus.Reset();

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
	USuperFAISSVectorBank* Bank = GetSelectedBank();
	if (Bank == nullptr || !Bank->IsValid())
	{
		ProjectionStatus = TEXT("no valid bank selected");
		return;
	}
	const BankView Full = Bank->GetBankView();

	// Channel scope: restrict the projected payload to one named channel's
	// sub-range (its own cluster structure). Offsets are grid-aligned, so the
	// slice is a straight per-row byte copy.
	int32 ScopeOffset = 0;
	int32 ScopeDims = Full.dims;
	FString ScopeLabel;
	if (SelectedProjectionScope.IsValid() &&
		*SelectedProjectionScope != TEXT("(whole row)"))
	{
		const int32 Channel = Bank->GetChannelIndex(FName(**SelectedProjectionScope));
		if (Channel == INDEX_NONE)
		{
			ProjectionStatus = TEXT("unknown channel scope");
			return;
		}
		ScopeOffset = Bank->ChannelOffsets[Channel];
		ScopeDims = Bank->ChannelLengths[Channel];
		ScopeLabel = FString::Printf(TEXT(", channel %s"), **SelectedProjectionScope);
	}
	const int32 ElemBytes = ElementSize(Full.quant);
	const int32 ScopePd = PaddedDims(ScopeDims, Full.quant);
	const int64 ScopeRowBytes = static_cast<int64>(ScopePd) * ElemBytes;
	const int64 CopyBytes = static_cast<int64>(ScopeDims) * ElemBytes;

	// Deterministic stride sample into a compact contiguous copy (N1: the tool is
	// bounded on any bank size; the copy keeps the core's contiguous-rows contract).
	const int32 Stride = FMath::Max(1, FMath::DivideAndRoundUp(Full.count,
		static_cast<int32>(ProjectionSampleLimit)));
	TArray<int32> Sampled;
	for (int32 R = 0; R < Full.count; R += Stride)
	{
		Sampled.Add(R);
	}
	const int64 FullRowBytes = RowBytes(Full);
	TArray<uint8, TAlignedHeapAllocator<16>> Payload;
	Payload.SetNumZeroed(Sampled.Num() * ScopeRowBytes);
	TArray<float> Scales;
	for (int32 S = 0; S < Sampled.Num(); ++S)
	{
		FMemory::Memcpy(Payload.GetData() + S * ScopeRowBytes,
			static_cast<const uint8*>(Full.rows) + Sampled[S] * FullRowBytes +
				static_cast<int64>(ScopeOffset) * ElemBytes,
			CopyBytes);
		if (Full.quant == Quantization::Int8)
		{
			Scales.Add(Full.scales[Sampled[S]]);
		}
	}
	BankView View = Full;
	View.rows = Payload.GetData();
	View.scales = Scales.Num() ? Scales.GetData() : nullptr;
	View.count = Sampled.Num();
	View.dims = ScopeDims;
	View.paddedDims = ScopePd;
	View.channels = nullptr;
	View.channelCount = 0;
	View.channelInvNorms = nullptr;

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
	ProjectionStatus = FString::Printf(TEXT("%d of %d rows%s%s"), View.count, Full.count,
		Stride > 1 ? TEXT(" (sampled)") : TEXT(""), *ScopeLabel);
}
