using UnrealBuildTool;

public class SuperFAISSUnrealDemo : ModuleRules
{
	public SuperFAISSUnrealDemo(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"Slate",
			"SlateCore",
			"SuperFAISSUnreal",
			"AssetRegistry",
		});
	}
}
