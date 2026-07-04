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
			// Station 2 (plan section 11, D13): the Mass swarm - the demo module is the
			// plugin's only Mass contact (section 6 strippable-by-construction rule).
			"MassEntity",
			"MassCore",
		});
	}
}
