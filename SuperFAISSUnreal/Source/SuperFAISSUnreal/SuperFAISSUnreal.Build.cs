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
		// under fast-math — verified at the compiler, not assumed. Emits -ffp-contract=off
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

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// UE::Trace::ToggleChannel (V3.2 slot 5, plugin plan section 5.1): Core
			// publicly depends on TraceLog for its own trace macros, but this module
			// links against Trace API entry points directly (the red suite's B8 non-
			// perturbation test toggles the SuperFAISS channel), which needs TraceLog
			// named explicitly to resolve at link time.
			"TraceLog",
		});
	}
}
