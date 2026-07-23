#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "SuperFAISSInspectorSettings.generated.h"

// V3.2 plan section 25.3 "Analysis parameters persist per-user, per-project (resolves
// E-D4-1)": one editor settings object, per-user-per-project (config =
// EditorPerProjectUserSettings), holding every analysis parameter for every panel of the
// whole Bank Inspector arc. 3.2 ships the seven fields named in the plan; sections 3.3-3.7
// add their own fields to this SAME object in later minors.
//
// The lifetime rule (stated once for the arc, section 25.3): analysis parameters (this
// object) persist; query state (channel weights, query text, selected banks) stays
// session-scoped and reset-on-select, exactly as today (V32-G2) — this object carries none
// of that.
//
// Fields carry their real plan-pinned defaults (section 25.3) so a fresh install reads
// the documented values. Every clamp accessor and EffectiveDefaultSample (section 25.9
// trust-boundary contract, audit G-1) is real, enforced clamping — CLOSED GREEN as of
// slot 4 (GetClampedMatchK, the last stub, was implemented this round).
//
// CslsMarginThreshold (SF34-006, plugin 3.3.1): pinned to the build-time CALIBRATION this
// class comment always said it needed (the Budget_MACs precedent: a measured constant a
// build task hands to the code, never a value the code derives or a red test asserts a
// specific number for) — no longer a placeholder. Population and basis, D-INSP-20
// (`Claude/Curie/superfaiss-3.3.1-test-design-2026-07-22.md` section 1): the hand-authored
// tutorial-bank correspondence set at MatchK=2 produces exactly two clusters — five clean
// singleton pairs at margin 0.5, and one near-duplicate collision (the deliberate
// ISO-B/ISO-B-dup fixture pair) at margin 0.25, correctly flagged lower-confidence by the
// margin's own definition. Any threshold strictly inside (0.25, 0.5] separates the two
// clusters correctly; 0.375 (the cluster midpoint) is pinned as the shipped default. This is
// a tutorial-population calibration, not a re-derivation at the shipped production MatchK
// (10) — the test-design's own scope note: the tutorial fixture's Secondary bank has only 6
// rows, so MatchK cannot exceed 6 there, and a margin distribution is a function of MatchK.
// Revisit when a real correspondence population large enough to calibrate at MatchK=10
// exists (R-2, plan section 8).
// No clamp accessor exists for this field, and none is added by this pin: unlike
// SampleLimit/StructureK/MinComponentSize/NoveltyK (feed array/buffer sizing — a hostile
// value is a memory-safety trust boundary, audit G-1) and NoveltyLambda (a documented [0,1]
// range), CslsMarginThreshold feeds only a float comparison (the matched/ambiguous
// classification) with no sizing exposure — a NaN/Inf value here is well-defined C++
// behavior (a comparison against NaN is simply always false, so a hostile threshold falls
// every otherwise-matched pair to Ambiguous, never Matched, never UB), so audit G-1's
// "never UB" trust-boundary reason does not reach this field the way it reaches the others.
UCLASS(config = EditorPerProjectUserSettings, defaultconfig,
	meta = (DisplayName = "SuperFAISS Bank Inspector"))
class SUPERFAISSUNREALEDITOR_API USuperFAISSInspectorSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	// The shared sample cap for every bank-wide Inspector pass (Structure, the Novelty
	// baseline): the deterministic strided sample construction's one shared cap
	// (section 25.3). Documented range: [1, kHardSampleCap] (the hard cap the panel copy
	// discloses as "a deliberate deeper look").
	UPROPERTY(config, EditAnywhere, Category = "Sampling")
	int32 SampleLimit = 2048;

	// View A (Structure): the mutual k-NN graph's k, within the brief's 16-20, >= log2 of
	// both the demo bank and the sample cap (section 25.5).
	UPROPERTY(config, EditAnywhere, Category = "Structure")
	int32 StructureK = 16;

	// View A (Structure): components smaller than this fold into the single labeled
	// outliers row (section 25.5).
	UPROPERTY(config, EditAnywhere, Category = "Structure")
	int32 MinComponentSize = 3;

	// View B (Novelty): the k-th-nearest-neighbour probe's k (section 25.5).
	UPROPERTY(config, EditAnywhere, Category = "Novelty")
	int32 NoveltyK = 8;

	// View B (Novelty): the CDF-rank threshold; documented range [0, 1] (section 25.5,
	// section 25.9 audit G-1).
	UPROPERTY(config, EditAnywhere, Category = "Novelty")
	float NoveltyLambda = 0.95f;

	// View C (Correspondence): the mutual-match retrieval width for both passes of
	// MutualNearestMatches (section 25.4/25.5) — plan-pinned default 10.
	UPROPERTY(config, EditAnywhere, Category = "Correspondence")
	int32 MatchK = 10;

	// View C (Correspondence): the CSLS-margin classification threshold (matched vs.
	// ambiguous, section 25.4). SF34-006 pin, 2026-07-22: 0.375, calibrated against the
	// tutorial-bank correspondence population (D-INSP-20) — see the class comment above for
	// the full basis. Valid anywhere in (0.25, 0.5] for that population; 0.375 is the
	// cluster midpoint.
	UPROPERTY(config, EditAnywhere, Category = "Correspondence")
	float CslsMarginThreshold = 0.375f;

	// The hard cap on SampleLimit (section 25.3: "the hard cap (8192) remains reachable as
	// a deliberate deeper look"). Not user-editable; the ceiling GetClampedSampleLimit
	// enforces.
	static constexpr int32 kHardSampleCap = 8192;

	// Clamped accessors (section 25.9 dim 2, audit G-1): the settings object is a trust
	// boundary — its values are user-editable ini config feeding array sizing, so every
	// field is read through a clamp that maps a hostile value to defined, in-range
	// behavior, never UB.
	int32 GetClampedSampleLimit() const;
	int32 GetClampedStructureK() const;
	int32 GetClampedMinComponentSize() const;
	int32 GetClampedNoveltyK() const;
	float GetClampedNoveltyLambda() const;
	// MatchK feeds the same array/workspace-sizing trust boundary as the other k-shaped
	// fields (StructureK, NoveltyK): floored at 1, no documented ceiling. No clamp accessor
	// exists for CslsMarginThreshold — see the class comment above (a NaN/Inf comparison is
	// already well-defined, never UB).
	int32 GetClampedMatchK() const;

	// The dims-aware sub-second default sample cap:
	// EffectiveDefaultSample = min(ClampedSampleLimit, floor(sqrt(BudgetMacs / Dims))).
	// `BudgetMacs` is the caller-supplied, calibrated budget constant (calibration
	// is a separate measurement task, not this function's concern — it is handed the
	// constant, never derives it).
	int32 EffectiveDefaultSample(int32 Dims, int64 BudgetMacs) const;
};
