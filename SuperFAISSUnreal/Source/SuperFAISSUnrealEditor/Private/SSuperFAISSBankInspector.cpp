#include "SSuperFAISSBankInspector.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "SuperFAISSSubsystem.h"
#include "SuperFAISSVectorBank.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
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
							ProjectedPoints.Reset();
							ProjectionStatus.Reset();
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
			TArray<FSuperFAISSHit> Hits;
			Subsystem->QuerySync(Bank, Query, Args, Hits);
			for (const FSuperFAISSHit& Hit : Hits)
			{
				ResultLines.Add(MakeShared<FString>(FString::Printf(
					TEXT("%-24s  score %.4f   margin %.4f"),
					Hit.Id.IsNone() ? *FString::Printf(TEXT("#%d"), Hit.Index)
					                : *Hit.Id.ToString(),
					Hit.Score, Hit.Margin)));
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

	// Deterministic stride sample into a compact contiguous copy (N1: the tool is
	// bounded on any bank size; the copy keeps the core's contiguous-rows contract).
	const int32 Stride = FMath::Max(1, FMath::DivideAndRoundUp(Full.count,
		static_cast<int32>(ProjectionSampleLimit)));
	TArray<int32> Sampled;
	for (int32 R = 0; R < Full.count; R += Stride)
	{
		Sampled.Add(R);
	}
	const int64 RowBytesCount = RowBytes(Full);
	TArray<uint8, TAlignedHeapAllocator<16>> Payload;
	Payload.SetNumUninitialized(Sampled.Num() * RowBytesCount);
	TArray<float> Scales;
	for (int32 S = 0; S < Sampled.Num(); ++S)
	{
		FMemory::Memcpy(Payload.GetData() + S * RowBytesCount,
			static_cast<const uint8*>(Full.rows) + Sampled[S] * RowBytesCount,
			RowBytesCount);
		if (Full.quant == Quantization::Int8)
		{
			Scales.Add(Full.scales[Sampled[S]]);
		}
	}
	BankView View = Full;
	View.rows = Payload.GetData();
	View.scales = Scales.Num() ? Scales.GetData() : nullptr;
	View.count = Sampled.Num();

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
	ProjectionStatus = FString::Printf(TEXT("%d of %d rows%s"), View.count, Full.count,
		Stride > 1 ? TEXT(" (sampled)") : TEXT(""));
}
