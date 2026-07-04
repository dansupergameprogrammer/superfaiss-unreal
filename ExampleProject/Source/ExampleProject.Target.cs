using UnrealBuildTool;

public class ExampleProjectTarget : TargetRules
{
	public ExampleProjectTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		BuildEnvironment = TargetBuildEnvironment.Shared;
		DefaultBuildSettings = BuildSettingsVersion.V7;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("ExampleProject");
	}
}
