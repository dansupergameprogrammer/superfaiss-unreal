using System.IO;
using UnrealBuildTool;

public class SuperFAISSUnreal : ModuleRules
{
	public SuperFAISSUnreal(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// The vendored SuperFAISS core forbids implicit FP contraction (its scalar/SIMD
		// bit-equality depends on exact mul-then-add rounding). Per-module precise FP is
		// the ONLY reliable mechanism: source pragmas do not stop clang backend fusion
		// under fast-math (Poirot S4, proven at the compiler). Emits -ffp-contract=off
		// on clang toolchains and /fp:precise on MSVC.
		FPSemantics = FPSemanticsMode.Precise;

		// The vendored SuperFAISS core is compiled into this module via the wrapper
		// files in Private/Vendored (see the float_control note there). Unity builds
		// are disabled so those wrappers keep their own compile environment.
		bUseUnity = false;

		PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "ThirdParty", "SuperFAISS", "include"));

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});
	}
}
