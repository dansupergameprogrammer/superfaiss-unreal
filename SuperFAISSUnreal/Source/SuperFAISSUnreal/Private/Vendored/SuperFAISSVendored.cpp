// Compiles the vendored SuperFAISS core into this module.
//
// FP policy: SuperFAISS forbids implicit FP contraction (scalar/SIMD bit-equality
// depends on exact mul-then-add rounding). The guarantee comes from the module-wide
// FPSemantics = Precise in Build.cs — NOT from source pragmas, which do not stop
// clang backend fusion under fast-math (verified at the compiler, Poirot S4).
// The SuperFAISS.B.SimdMirrorEquality automation test is the standing tripwire
// on every platform.

#include "../../../ThirdParty/SuperFAISS/src/kernels.cpp"
#include "../../../ThirdParty/SuperFAISS/src/kernels_avx2.cpp"
#include "../../../ThirdParty/SuperFAISS/src/alloc.cpp"
#include "../../../ThirdParty/SuperFAISS/src/validate.cpp"
#include "../../../ThirdParty/SuperFAISS/src/bake.cpp"
#include "../../../ThirdParty/SuperFAISS/src/query.cpp"
#include "../../../ThirdParty/SuperFAISS/src/compose.cpp"
#include "../../../ThirdParty/SuperFAISS/src/analytics.cpp"
#include "../../../ThirdParty/SuperFAISS/src/pca.cpp"
#include "../../../ThirdParty/SuperFAISS/src/scratch.cpp"