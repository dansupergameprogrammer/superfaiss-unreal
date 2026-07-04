#include "Modules/ModuleManager.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "SSuperFAISSBankInspector.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

// Registers the 18.2 inspection tab: Tools > SuperFAISS Bank Inspector — the live
// query inspector and the PCA projection visualizer in one dockable tab.
class FSuperFAISSUnrealEditorModule : public IModuleInterface
{
public:
	static const FName TabName;

	virtual void StartupModule() override
	{
		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabName,
			FOnSpawnTab::CreateStatic(&FSuperFAISSUnrealEditorModule::SpawnTab))
			.SetDisplayName(NSLOCTEXT("SuperFAISS", "InspectorTab",
				"SuperFAISS Bank Inspector"))
			.SetTooltipText(NSLOCTEXT("SuperFAISS", "InspectorTabTip",
				"Inspect bank assets: live queries with margins, PCA projection"))
			.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory());
	}

	virtual void ShutdownModule() override
	{
		if (FSlateApplication::IsInitialized())
		{
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabName);
		}
	}

private:
	static TSharedRef<SDockTab> SpawnTab(const FSpawnTabArgs& Args)
	{
		return SNew(SDockTab).TabRole(ETabRole::NomadTab)
		[
			SNew(SSuperFAISSBankInspector)
		];
	}
};

const FName FSuperFAISSUnrealEditorModule::TabName(TEXT("SuperFAISSBankInspector"));

IMPLEMENT_MODULE(FSuperFAISSUnrealEditorModule, SuperFAISSUnrealEditor)
