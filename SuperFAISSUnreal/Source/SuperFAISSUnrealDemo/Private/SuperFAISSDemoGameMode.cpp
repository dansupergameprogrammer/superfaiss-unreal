#include "SuperFAISSDemoGameMode.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformTime.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "SuperFAISSSubsystem.h"
#include "SuperFAISSVectorBank.h"

// ---------------------------------------------------------------------------
// Station UI, side-by-side edition: one query, two banks, compared live. Pure Slate,
// no widget assets. Station 2 (Mass swarm) is the recorded next pass.

class SSuperFAISSUnrealDemo : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSuperFAISSUnrealDemo) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, TArray<USuperFAISSVectorBank*> InBanks)
	{
		for (USuperFAISSVectorBank* InBank : InBanks)
		{
			if (InBank != nullptr && InBank->IsValid())
			{
				Banks.Add(InBank);
				BankNames.Add(MakeShared<FString>(FString::Printf(TEXT("%s  (%dd)"),
					*InBank->GetName(), InBank->Dims)));
			}
		}
		// Default: different banks per side when we have two.
		SetPanelBank(0, 0);
		SetPanelBank(1, Banks.Num() > 1 ? 1 : 0);

		const FSlateFontInfo Title = FCoreStyle::GetDefaultFontStyle("Bold", 22);
		const FSlateFontInfo Body = FCoreStyle::GetDefaultFontStyle("Regular", 14);

		ChildSlot
		[
			SNew(SBorder)
			.Padding(24.0f)
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
				[
					SNew(STextBlock).Font(Title)
					.Text(FText::FromString(TEXT("SuperFAISSUnreal — one query, two banks")))
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 12, 0)
					[
						SNew(SBox).WidthOverride(420.0f)
						[
							SNew(SEditableTextBox)
							.Font(Body)
							.HintText(FText::FromString(TEXT("type a word — lion, wizard, guitar, reno...")))
							.OnTextChanged(this, &SSuperFAISSUnrealDemo::OnQueryTextChanged)
						]
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.OnClicked(this, &SSuperFAISSUnrealDemo::OnRunBenchmark)
						[
							SNew(STextBlock).Font(Body)
							.Text(FText::FromString(TEXT("Benchmark both banks")))
						]
					]
				]

				+ SVerticalBox::Slot().FillHeight(1.0f).Padding(0, 10, 0, 0)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(0.5f).Padding(0, 0, 10, 0)
					[
						MakePanel(0, Body)
					]
					+ SHorizontalBox::Slot().FillWidth(0.5f).Padding(10, 0, 0, 0)
					[
						MakePanel(1, Body)
					]
				]
			]
		];
	}

private:
	struct FPanel
	{
		int32 ActiveIndex = 0;
		TWeakObjectPtr<USuperFAISSVectorBank> Bank;
		TMap<FName, int32> WordToRow;
		FString TimingLine;
		FString ResultsLines;
		FString BenchLines;
		uint64 QueryGeneration = 0;
	};

	TSharedRef<SWidget> MakePanel(int32 PanelIndex, const FSlateFontInfo& Body)
	{
		const FSlateFontInfo Mono = FCoreStyle::GetDefaultFontStyle("Mono", 13);
		return SNew(SBorder)
			.Padding(12.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
				[
					SNew(SBox).WidthOverride(300.0f)
					[
						SNew(SComboBox<TSharedPtr<FString>>)
						.OptionsSource(&BankNames)
						.OnSelectionChanged(
							SComboBox<TSharedPtr<FString>>::FOnSelectionChanged::CreateSP(
								this, &SSuperFAISSUnrealDemo::OnBankSelected, PanelIndex))
						.OnGenerateWidget_Lambda([Body](TSharedPtr<FString> Item)
						{
							return SNew(STextBlock).Font(Body).Text(FText::FromString(*Item));
						})
						[
							SNew(STextBlock).Font(Body)
							.Text_Lambda([this, PanelIndex]()
							{
								return BankNames.IsValidIndex(Panels[PanelIndex].ActiveIndex)
									? FText::FromString(*BankNames[Panels[PanelIndex].ActiveIndex])
									: FText::FromString(TEXT("no banks found"));
							})
						]
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
				[
					SNew(STextBlock).Font(Body).AutoWrapText(true)
					.Text_Lambda([this, PanelIndex]() { return GetPanelSummary(PanelIndex); })
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
				[
					SNew(STextBlock).Font(Mono)
					.Text_Lambda([this, PanelIndex]()
					{
						return FText::FromString(Panels[PanelIndex].TimingLine);
					})
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
				[
					SNew(STextBlock).Font(Mono)
					.Text_Lambda([this, PanelIndex]()
					{
						return FText::FromString(Panels[PanelIndex].ResultsLines);
					})
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Font(Mono)
					.Text_Lambda([this, PanelIndex]()
					{
						return FText::FromString(Panels[PanelIndex].BenchLines);
					})
				]
			];
	}

	FText GetPanelSummary(int32 PanelIndex) const
	{
		const FPanel& Panel = Panels[PanelIndex];
		if (!Panel.Bank.IsValid() || !Panel.Bank->IsValid())
		{
			return FText::FromString(TEXT("Bank missing or invalid."));
		}
		return FText::FromString(FString::Printf(
			TEXT("%d words x %d dims, int8, %.1f MB, recall@10 %.4f"),
			Panel.Bank->Count, Panel.Bank->Dims,
			Panel.Bank->GetPayloadBytes() / (1024.0 * 1024.0), Panel.Bank->RecallAt10));
	}

	void SetPanelBank(int32 PanelIndex, int32 BankIndex)
	{
		FPanel& Panel = Panels[PanelIndex];
		if (!Banks.IsValidIndex(BankIndex) || !Banks[BankIndex].IsValid())
		{
			Panel.Bank = nullptr;
			return;
		}
		Panel.ActiveIndex = BankIndex;
		Panel.Bank = Banks[BankIndex];
		Panel.WordToRow.Reset();
		for (int32 i = 0; i < Panel.Bank->Ids.Num(); ++i)
		{
			Panel.WordToRow.Add(Panel.Bank->Ids[i], i);
		}
		Panel.TimingLine.Reset();
		Panel.ResultsLines.Reset();
		Panel.BenchLines.Reset();
		RunPanelQuery(PanelIndex);
	}

	void OnBankSelected(TSharedPtr<FString> Item, ESelectInfo::Type, int32 PanelIndex)
	{
		const int32 Index = BankNames.IndexOfByKey(Item);
		if (Index != INDEX_NONE)
		{
			SetPanelBank(PanelIndex, Index);
		}
	}

	static TArray<float> RowVector(const USuperFAISSVectorBank* Bank, int32 Row)
	{
		using namespace superfaiss;
		const BankView View = Bank->GetBankView();
		const int8* RowData = static_cast<const int8*>(View.rows) +
			static_cast<int64>(Row) * View.paddedDims;
		const float Scale = View.scales[Row];
		TArray<float> Query;
		Query.SetNumUninitialized(Bank->Dims);
		for (int32 i = 0; i < Bank->Dims; ++i)
		{
			Query[i] = Scale * RowData[i];
		}
		return Query;
	}

	void OnQueryTextChanged(const FText& NewText)
	{
		CurrentWord = NewText.ToString().TrimStartAndEnd().ToLower();
		RunPanelQuery(0);
		RunPanelQuery(1);
	}

	void RunPanelQuery(int32 PanelIndex)
	{
		FPanel& Panel = Panels[PanelIndex];
		if (!Panel.Bank.IsValid() || !Panel.Bank->IsValid())
		{
			return;
		}
		if (CurrentWord.IsEmpty())
		{
			Panel.TimingLine.Reset();
			Panel.ResultsLines.Reset();
			return;
		}
		const int32* Row = Panel.WordToRow.Find(FName(*CurrentWord));
		if (Row == nullptr)
		{
			Panel.TimingLine = FString::Printf(TEXT("'%s' is not in this bank"), *CurrentWord);
			Panel.ResultsLines.Reset();
			return;
		}

		USuperFAISSVectorBank* Bank = Panel.Bank.Get();
		ExcludeScratch.Init(0, (Bank->Count + 31) / 32);
		ExcludeScratch[*Row >> 5] |= 1u << (*Row & 31);

		FSuperFAISSQueryArgs Args;
		Args.K = 10;
		Args.ExcludeBits = ExcludeScratch;

		const double Start = FPlatformTime::Seconds();
		const uint64 Generation = ++Panel.QueryGeneration;

		FSuperFAISSNativeResultDelegate Done;
		Done.BindSP(this, &SSuperFAISSUnrealDemo::OnQueryComplete, PanelIndex, Start, Generation);
		GEngine->GetEngineSubsystem<USuperFAISSSubsystem>()->QueryAsync(
			Bank, RowVector(Bank, *Row), Args, MoveTemp(Done));
	}

	void OnQueryComplete(const TArray<FSuperFAISSHit>& Hits, bool bSuccess,
		int32 PanelIndex, double Start, uint64 Generation)
	{
		FPanel& Panel = Panels[PanelIndex];
		// Keystrokes race their queries; only the latest dispatched query may render.
		if (Generation != Panel.QueryGeneration || !bSuccess)
		{
			return;
		}
		const double Ms = (FPlatformTime::Seconds() - Start) * 1000.0;
		Panel.TimingLine = FString::Printf(TEXT("%.3f ms (async)"), Ms);
		Panel.ResultsLines.Reset();
		for (int32 i = 0; i < Hits.Num(); ++i)
		{
			Panel.ResultsLines += FString::Printf(TEXT("%2d. %-24s %.4f\n"),
				i + 1, *Hits[i].Id.ToString(), Hits[i].Score);
		}
	}

	FReply OnRunBenchmark()
	{
		USuperFAISSSubsystem* Sim = GEngine->GetEngineSubsystem<USuperFAISSSubsystem>();
		for (FPanel& Panel : Panels)
		{
			if (!Panel.Bank.IsValid() || !Panel.Bank->IsValid())
			{
				continue;
			}
			USuperFAISSVectorBank* Bank = Panel.Bank.Get();

			constexpr int32 BatchWidth = 64;
			FSuperFAISSQueryArgs Args;
			Args.K = 10;

			TArray<float> Queries;
			Queries.Reserve(BatchWidth * Bank->Dims);
			for (int32 M = 0; M < BatchWidth; ++M)
			{
				Queries.Append(RowVector(Bank, (M * 307) % Bank->Count));
			}
			TArray<FSuperFAISSHit> Hits;
			TArray<int32> Counts;

			Sim->QuerySync(Bank, TConstArrayView<float>(Queries.GetData(), Bank->Dims), Args, Hits);
			Sim->QueryBatch(Bank, Queries, BatchWidth, Args, Hits, Counts);

			double BestSingle = 1e300;
			for (int32 Run = 0; Run < 5; ++Run)
			{
				const double T0 = FPlatformTime::Seconds();
				for (int32 R = 0; R < 20; ++R)
				{
					Sim->QuerySync(Bank,
						TConstArrayView<float>(Queries.GetData() + (R % BatchWidth) * Bank->Dims, Bank->Dims),
						Args, Hits);
				}
				BestSingle = FMath::Min(BestSingle, (FPlatformTime::Seconds() - T0) / 20.0);
			}
			double BestBatch = 1e300;
			for (int32 Run = 0; Run < 5; ++Run)
			{
				const double T0 = FPlatformTime::Seconds();
				Sim->QueryBatch(Bank, Queries, BatchWidth, Args, Hits, Counts);
				BestBatch = FMath::Min(BestBatch, FPlatformTime::Seconds() - T0);
			}

			Panel.BenchLines = FString::Printf(
				TEXT("single    %8.3f ms\nbatch x%d %8.3f ms  (%.3f ms/query)\n@60fps: one query = %.2f%% of a frame"),
				BestSingle * 1e3,
				BatchWidth, BestBatch * 1e3, BestBatch * 1e3 / BatchWidth,
				BestSingle * 100.0 / 0.0166667);
		}
		return FReply::Handled();
	}

	TArray<TWeakObjectPtr<USuperFAISSVectorBank>> Banks;
	TArray<TSharedPtr<FString>> BankNames;
	FPanel Panels[2];
	FString CurrentWord;
	TArray<uint32> ExcludeScratch;
};

// ---------------------------------------------------------------------------

void ASuperFAISSDemoGameMode::BeginPlay()
{
	Super::BeginPlay();

	// Discover every bank in the demo folder — drop a new bank asset in and it
	// appears in both panels' switchers. In an uncooked -game launch the asset
	// registry populates asynchronously and has not reached plugin content by
	// BeginPlay, so scan the demo path synchronously first. bForceRescan=false: the
	// real target (an unscanned -game registry) is scanned identically, and PIE (where
	// the path is already scanned at editor startup) skips the redundant rework.
	FAssetRegistryModule& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	AssetRegistry.Get().ScanPathsSynchronous({TEXT("/SuperFAISSUnreal/Demo")}, /*bForceRescan*/ false);

	TArray<FAssetData> Assets;
	AssetRegistry.Get().GetAssetsByPath(TEXT("/SuperFAISSUnreal/Demo"), Assets, true);
	for (const FAssetData& Asset : Assets)
	{
		if (Asset.AssetClassPath == USuperFAISSVectorBank::StaticClass()->GetClassPathName())
		{
			if (USuperFAISSVectorBank* Bank = Cast<USuperFAISSVectorBank>(Asset.GetAsset()))
			{
				DemoBanks.Add(Bank);
			}
		}
	}

	// Fallback: if discovery still yields nothing (registry unavailable in this
	// launch config), load the known demo banks directly so the demo can never boot
	// bankless. Direct LoadObject does not depend on the registry.
	//
	// MAINTENANCE COUPLING (Poirot O1): this list is the *emergency* path only — the
	// registry path above is what makes "drop a bank in the folder and it appears"
	// true. This hardcoded set must be kept in step with the banks baked by the
	// SuperFAISSUnrealBake commandlet (the source of truth). If a new bank is baked
	// and NOT added here, it appears in every normal launch but silently vanishes in
	// any stripped config that falls through to this fallback.
	if (DemoBanks.Num() == 0)
	{
		static const TCHAR* KnownBanks[] = {
			TEXT("/SuperFAISSUnreal/Demo/DemoBank.DemoBank"),
			TEXT("/SuperFAISSUnreal/Demo/DemoBank300.DemoBank300"),
		};
		for (const TCHAR* Path : KnownBanks)
		{
			if (USuperFAISSVectorBank* Bank = LoadObject<USuperFAISSVectorBank>(nullptr, Path))
			{
				DemoBanks.Add(Bank);
			}
		}
	}

	DemoBanks.Sort([](const USuperFAISSVectorBank& A, const USuperFAISSVectorBank& B)
	{
		return A.GetName() < B.GetName();
	});

	UE_LOG(LogTemp, Display, TEXT("SuperFAISSUnrealDemo: %d bank(s) available"), DemoBanks.Num());
	for (const USuperFAISSVectorBank* Bank : DemoBanks)
	{
		UE_LOG(LogTemp, Display, TEXT("  %s (%dd, %d words, valid=%d)"),
			*Bank->GetName(), Bank->Dims, Bank->Count, Bank->IsValid() ? 1 : 0);
	}

	DemoWidget = SNew(SSuperFAISSUnrealDemo, TArray<USuperFAISSVectorBank*>(DemoBanks));
	if (GEngine->GameViewport)
	{
		GEngine->GameViewport->AddViewportWidgetContent(DemoWidget.ToSharedRef(), 100);
	}
	if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		PC->bShowMouseCursor = true;
		PC->SetInputMode(FInputModeUIOnly());
	}
}

void ASuperFAISSDemoGameMode::EndPlay(const EEndPlayReason::Type Reason)
{
	if (DemoWidget.IsValid() && GEngine && GEngine->GameViewport)
	{
		GEngine->GameViewport->RemoveViewportWidgetContent(DemoWidget.ToSharedRef());
	}
	DemoWidget.Reset();
	Super::EndPlay(Reason);
}
