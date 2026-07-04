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
		});
	}
}
