using UnrealBuildTool;

public class ExampleProjectEditorTarget : TargetRules
{
	public ExampleProjectEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		BuildEnvironment = TargetBuildEnvironment.Shared;
		DefaultBuildSettings = BuildSettingsVersion.V7;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("ExampleProject");
	}
}
