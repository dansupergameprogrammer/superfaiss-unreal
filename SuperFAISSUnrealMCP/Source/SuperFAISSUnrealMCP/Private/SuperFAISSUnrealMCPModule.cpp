#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"

#include "SuperFAISSToolset.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

// Registers the SuperFAISS toolset with the engine's ToolsetRegistry, from which the
// MCP plugin's adapter discovers it (M-G3 — this module never depends on the MCP
// plugin itself). Compile-out on stock engines is the plugin enable switch: this
// plugin ships disabled-by-default with a hard ToolsetRegistry dependency (§19.3 as
// amended, D-M4).
//
// Registration is deferred to OnPostEngineInit: the registry's backing editor
// subsystem does not exist at module-load time (observed at the M-V1 gate —
// "AIToolsetRegistrySubsystem unavailable" when registering from StartupModule).
class FSuperFAISSUnrealMCPModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddLambda([]()
		{
			UToolsetRegistry::RegisterToolsetClass(USuperFAISSToolset::StaticClass());
		});
	}

	virtual void ShutdownModule() override
	{
		FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
		if (UObjectInitialized())
		{
			UToolsetRegistry::UnregisterToolsetClass(USuperFAISSToolset::StaticClass());
		}
	}

private:
	FDelegateHandle PostEngineInitHandle;
};

IMPLEMENT_MODULE(FSuperFAISSUnrealMCPModule, SuperFAISSUnrealMCP)
