using UnrealBuildTool;

public class SuperFAISSUnrealEditor : ModuleRules
{
	public SuperFAISSUnrealEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseUnity = false;

		// This module compiles its own copy of the vendored SuperFAISS core (see
		// Private/Vendored), so it carries the same FP contract as the runtime module:
		// no implicit FP contraction. See SuperFAISSUnreal.Build.cs for the full note.
		FPSemantics = FPSemanticsMode.Precise;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"SuperFAISSUnreal",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Json",
			"UnrealEd",
			// Inspector/visualizer tab (plan section 18.2).
			"Slate",
			"SlateCore",
			"InputCore",
			"WorkspaceMenuStructure",
			// USuperFAISSInspectorSettings, UDeveloperSettings (V3.2 plan section 25.3).
			"DeveloperSettings",
			// IPluginManager, for the slot-3 grep-target regression test's module-dir lookup.
			"Projects",
			// UE::Trace::ToggleChannel (V3.2 slot 5, plugin plan section 5.1/25.6): the
			// B8-extension non-perturbation test toggles the SuperFAISS channel; needs
			// TraceLog named explicitly to resolve at link time (see the runtime
			// module's Build.cs for the identical note).
			"TraceLog",
		});
	}
}
