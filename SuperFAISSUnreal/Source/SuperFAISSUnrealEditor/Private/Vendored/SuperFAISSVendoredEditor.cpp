// The editor module compiles its own copy of the vendored core: the runtime module
// does not export SuperFAISS symbols (an upstream SUPERFAISS_API export macro is the
// recorded follow-up; its deadline is any monolithic build configuration). Per-DLL
// statics (allocation counters, AVX2 detection) are duplicated and independent, which
// is acceptable for editor-side import/validation.
//
// FP policy: module-wide FPSemantics = Precise in Build.cs (flags, not pragmas — see
// the runtime wrapper's note).

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

// V3.2 Bank Inspector I, Tier 1 (plan section 25.4) — the editor module's own copy,
// same reason as the block above: no cross-module export exists yet.
#include "../../../ThirdParty/SuperFAISS/src/graph.cpp"
#include "../../../ThirdParty/SuperFAISS/src/novelty.cpp"
#include "../../../ThirdParty/SuperFAISS/src/matching.cpp"