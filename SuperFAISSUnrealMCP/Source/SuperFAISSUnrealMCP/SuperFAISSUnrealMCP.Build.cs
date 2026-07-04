using UnrealBuildTool;

public class SuperFAISSUnrealMCP : ModuleRules
{
	public SuperFAISSUnrealMCP(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// This plugin is disabled by default and hard-depends on the Experimental
		// ToolsetRegistry engine plugin (marked not-for-redistribution; absent from stock distributions).
		// Compile-out is the plugin enable switch itself (plan section 19.3 as amended,
		// D-M4): stock-engine users never enable it, so this module never builds there;
		// a preprocessor stub is impossible anyway - UHT forbids reflected types inside
		// preprocessor blocks, and the toolset class must be reflected.
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"AssetRegistry",
			"Json",
			"SuperFAISSUnreal",
			"SuperFAISSUnrealEditor",
			"ToolsetRegistry",
			"UnrealEd",
		});
	}
}
