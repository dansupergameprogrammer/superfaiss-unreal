#pragma once

#include "CoreMinimal.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SCompoundWidget.h"

#include "superfaiss/types.h"

// V3.2 slot 4b: the inspection-source abstraction (FSuperFAISSInspectionSource, below)
// needs the bank enum TYPES (ESuperFAISSBankMetric/ESuperFAISSBankQuantization) by value
// in its own accessor signatures, not just a forward-declared class -- promoted from the
// prior forward-declare-only posture to a real include.
#include "SuperFAISSVectorBank.h"
// A real include, not a forward-declare: TStrongObjectPtr<USuperFAISSScratchBank> is a
// widget-class member (PrimaryArchiveBank/SecondArchiveBank below), and its implicit
// special members (constructed/destroyed wherever a TSharedRef<SSuperFAISSBankInspector>
// is, including every automation test's SNew(...) call) need the complete type in EVERY
// translation unit that includes this header, not only this widget's own .cpp.
#include "SuperFAISSScratchBank.h"

// V3.2 plan section 25.9 dim 10 (Panel FEAT, audit G-6): one component of the sample the
// Structure pass ran on. `MemberSampleIndices` are indices into the SAME sampled view
// `ProjectedPoints` renders, so a component id joins 1:1 onto the rendered scatter (the
// "cluster coloring is a 1:1 join" load-bearing consequence, section 25.3).
struct FSuperFAISSStructureCluster
{
	int32 ComponentId = INDEX_NONE;
	TArray<int32> MemberSampleIndices;
};

// UI-only tree node for the cluster list (section 25.5: "members by Id where the bank
// carries Ids, else #index" — a cluster's own component id is NOT the user-facing label;
// it is the canonical smallest sample-index ConnectedComponents assigns the group, an
// arbitrary but deterministic integer, not a meaningful name). A header node (a cluster
// summary, or the Outliers row) has SampleIndex == INDEX_NONE and one child per member; a
// leaf node is one member row, labeled by its actual bank id (or #index) and independently
// selectable ("selecting a row highlights its component + its k nearest").
struct FSuperFAISSStructureListItem
{
	FString DisplayText;
	int32 SampleIndex = INDEX_NONE; // leaf only
	TArray<int32> HeaderMemberSampleIndices; // header only: every member's sample index
	TArray<TSharedPtr<FSuperFAISSStructureListItem>> Children;
};

// A row-select highlight is three DIFFERENT claims about a point (its component, that it's
// a k-nearest neighbor of the selected row, that it IS the selected row), and they render
// as one undifferentiated blob if painted alike — component ∪ k-nearest looks identical to
// "everything in the group" the moment a component is much bigger than k, which is most of
// the time. Priority order when a sample qualifies for more than one (common: a row's own
// k-NN usually sit inside its own component): Selected > Neighbor > Component.
enum class ESuperFAISSStructureHighlight : uint8
{
	Component,
	Neighbor,
	Selected,
};

// V3.2 plan section 25.5 View B: the tri-state verdict plus Dot's own rejection state,
// plus a fifth, RED-SCAFFOLD-ONLY sentinel. `Unavailable` is the Dot-bank status
// ("novelty verdict unavailable on Dot banks (dot product is not a dissimilarity)",
// section 25.9 dim 5 cycle-6 audit S1) — a status, not a fourth "real" verdict; it never
// carries a score. `NotComputed` is never a value section 25.5's spec produces — it is
// this round's scaffold default, a distinct poison value so a red cell asserting one of
// the four REAL outcomes can never coincidentally match an unwritten result (the M1-M3
// core-module "distinct poison" convention, applied here at the enum level since a
// struct field can't poison itself to an out-of-band int the way `outIds[r] = -(r+2)`
// does for a plain int32 array).
enum class ESuperFAISSNoveltyVerdict : uint8
{
	NotComputed,
	Duplicate,
	Novel,
	Familiar,
	Unavailable,
};

// V3.2 plan section 25.5 View B: the probe readout. `Score` is limb 2's CDF rank
// (`novelty vs N of M sampled rows`) and is meaningless when `Verdict` is `Duplicate`
// (the identity limb never consults the CDF, section 25.9 dim 7) or `Unavailable`.
// `bLowConfidence` is section 25.5's "effective sample below 64 rows" marking, scoped to
// limb-2 verdicts only (a `Duplicate` verdict is exact-evidence and never carries it).
struct FSuperFAISSNoveltyResult
{
	bool bValid = false;
	ESuperFAISSNoveltyVerdict Verdict = ESuperFAISSNoveltyVerdict::NotComputed;
	float Score = 0.0f;
	bool bLowConfidence = false;
	int32 SampledCount = 0;
	int32 TotalCount = 0;
	// The disclosure requirement: on a channel-scoped Cosine bank, how many rows the
	// limb-2 baseline sample dropped for zero energy in the scoped channel -- 0 whenever
	// the scope isn't a zero-energy-capable channel-scoped Cosine pass. SampledCount
	// already reflects the smaller, post-exclusion sample; this is why it is smaller.
	int32 ZeroEnergyExcludedCount = 0;
	// Set only when Verdict == Unavailable — the exact panel copy (section 25.5).
	FString UnavailableStatus;
};

// V3.2 plan section 25.5 View C: a matched pair's classification, CALLER-composed from
// MatchPair's raw cslsMargin against USuperFAISSInspectorSettings::CslsMarginThreshold
// (matching.h's own doc: "Classification... is CALLER-composed... no separate 'verdict'
// entry exists here either" — the widget is that caller). `NotComputed` is the RED-
// SCAFFOLD-ONLY poison sentinel (never a value section 25.5's spec produces), mirroring
// ESuperFAISSNoveltyVerdict's identical convention at the enum level.
enum class ESuperFAISSMatchState : uint8
{
	NotComputed,
	Unmatched,
	Ambiguous,
	Matched,
};

// V3.2 plan section 25.5 View C: one row of the matched-pair list. `SourceIndexA` is
// always the native A source index (populated for EVERY checked sample row, matched or
// not — matching.h's own `outPairs` contract); `SourceIndexB` is the native B source
// index, or -1 when `State == Unmatched`. `CslsMargin` is defined only when
// `SourceIndexB >= 0` (matching.h's own contract — 0.0f otherwise).
struct FSuperFAISSMatchPairResult
{
	int32 SourceIndexA = INDEX_NONE;
	int32 SourceIndexB = INDEX_NONE;
	float CslsMargin = 0.0f;
	ESuperFAISSMatchState State = ESuperFAISSMatchState::NotComputed;
};

// The inspection source is an abstraction: a baked asset OR a loaded scratch archive --
// "the one genuine widget refactor". Every BankView-native pass (BuildAnalysisSample,
// ComputeStructure, ComputeCorrespondence) reads a source through this uniform surface
// instead of reaching for USuperFAISSVectorBank directly, so the same pass code runs
// against either an asset-registry bank or a transient archive-loaded ScratchBank.
//
// FORCED READING: the design names the abstraction and its two
// space-law placements (construction-time discharge for sample-scoped passes, runtime OR
// for full-view passes) but not its concrete C++ shape. This is a value type the widget
// resolves per slot (primary / second, extended to the
// second-bank slot too -- "archive-vs-baked correspondence... exercised in slot A and in
// slot B separately"): EITHER a TWeakObjectPtr to a registry asset (unowned -- the asset
// registry itself keeps it alive, the existing pattern) OR a TStrongObjectPtr to a
// transient USuperFAISSScratchBank the widget itself constructs via NewObject +
// LoadFromBytes (an OWNED reference -- nothing else roots a transient scratch bank the
// way the registry roots an asset asset, or the way a TWeakObjectPtr would let it GC out
// from under the widget the moment no other strong ref exists). The asset picker and the
// "Open scratch archive..." affordance are mutually exclusive per slot (a further forced
// reading, since the design states the affordance exists beside the picker but not their
// interaction): opening an archive clears that slot's asset-combo selection, and picking
// an asset from the combo clears that slot's open archive (OnBankSelected() /
// OnSecondBankSelected(), extended).
//
// ASYMMETRY, stated ("your abstraction needs to account for
// this asymmetry, not paper over it"): an asset carries NO tombstones (baked banks are
// immutable -- GetTombstoneWords() is always empty there); an archive ALWAYS carries
// tombstone words from Snapshot(), even when every row happens to be live (an all-zero
// bitset, still real, still sized). An archive also carries NO ID TABLE (ScratchBank has
// no id storage at all, unlike USuperFAISSVectorBank::Ids) -- GetIdForIndex/GetIndexForId
// are always NAME_None/INDEX_NONE for an Archive-kind source, a real, defined, disclosed
// degrade to "#index"-only row addressing on an archive, not a silent wrong answer.
struct FSuperFAISSInspectionSource
{
	enum class EKind : uint8 { None, Asset, Archive };

	EKind Kind = EKind::None;
	TWeakObjectPtr<USuperFAISSVectorBank> Asset;
	TStrongObjectPtr<USuperFAISSScratchBank> ArchiveBank;
	// An archive source's display name (its opened filename) -- an archive carries no
	// UObject asset name to fall back on. Unused for Asset-kind (DisplayName() reads the
	// asset's own GetName() there).
	FString ArchiveDisplayName;

	bool IsValid() const;
	FString DisplayName() const;
	// Published row count (source space) -- what the source's own header/count declares.
	int32 GetCount() const;
	// Published minus tombstoned. Equals GetCount() for an asset (no tombstones exist,
	// the documented asymmetry); ScratchBank::LiveCount() for an archive.
	int32 GetLiveCount() const;
	int32 GetDims() const;
	ESuperFAISSBankMetric GetMetric() const;
	ESuperFAISSBankQuantization GetQuantization() const;
	int32 GetChannelCount() const;
	int32 GetChannelIndex(FName Name) const;
	FName GetIdForIndex(int32 Index) const;
	int32 GetIndexForId(FName Id) const;
	// The source-space BankView (rows/scales point directly at the asset's OR the scratch
	// bank's own storage -- no copy). A zeroed view if !IsValid().
	superfaiss::BankView GetBankView() const;
	// The space law (section 25.3): source-space tombstone words, TombstoneWords(GetCount())
	// entries -- ALWAYS EMPTY for an asset; the scratch bank's own Snapshot() words for an
	// archive (real even when nothing is tombstoned -- an all-zero, correctly-sized
	// bitset, not an empty/absent one, so a caller ORing these never needs a kind check).
	TArray<uint32> GetTombstoneWords() const;
};

// Determinism tier: PER-DEVICE for every V3.2 analysis pass (Structure, Novelty,
// Correspondence) -- fixed sample, fixed order, pinned tie-breaks, inherited directly
// from graph.h/novelty.h/matching.h's own tier (superfaiss.h). Layouts, cluster ids,
// and match pairings may differ across machines; there is no cross-device claim. This
// is the SAME disclosure the panel copy states to the user at runtime
// (StructureDisclosureCopy()) -- stated here too so a reader of this header, not only
// the UI, gets it without opening the widget.
//
// The 18.2 inspection surface, one dockable editor tab with two panes:
// - Live query inspector: pick a bank, type a row id (or index), see ranked matches
//   with margins — the demo's typed-word mode as a reusable editor tool. No encoder:
//   text works only on banks whose ids are the vocabulary (the 18.4 boundary rule).
//   On channel banks (schema 2) a weight slider per named channel drives the query,
//   and every hit carries decomposition bars — per-channel contributions from
//   DecomposeHit, which sum exactly to the score (V2 plan section 6). Displayed
//   per-channel cosines clamp to [-1, 1] (T-044 W2d: int8 quantization noise can
//   push a shade past 1; the clamp is display-only and marked when it fires).
// - Projection visualizer: PCA point cloud of the bank (2 components), computed on
//   demand over a deterministic stride sample (N1: bounded, never silently
//   exhaustive on a large bank). On channel banks the projection can be scoped to
//   one named channel's sub-range — that channel's own cluster structure.
// - View A (Structure) / View B (Novelty probe), V3.2 plan section 25.5: extend this
//   widget in place (E-R1). Both ride the SAME sample-and-scope construction the
//   projection uses (section 25.3's "one shared cap" + "same sample" — the projection
//   scope combo IS the one analysis-scope selector every sample-scoped pass reads; no
//   separate per-view scope control exists — a design reading this round makes
//   explicit because no new scope UI is named anywhere in section 25.5, mirroring the
//   M3 test-design round's own forced-reading disclosures). Analysis PARAMETERS
//   (k's, lambda, the sample cap) come from USuperFAISSInspectorSettings and persist;
//   query state (the probe text, which bank is selected) stays session-scoped exactly
//   as today (V32-G2).
// - View C (Correspondence), section 25.5 (slot 4): a second-bank slot alongside the
//   primary, matching rows between two banks via matching.h's mutual-NN + CSLS. Every
//   BankView-native pass -- including this one -- reads its bank(s) through
//   FSuperFAISSInspectionSource (below), which generalizes "a registry asset" to
//   "an asset OR a transient, archive-loaded USuperFAISSScratchBank" -- the
//   "archive-vs-baked correspondence" field-debugging
//   case (a player's saved memory bank against the shipped reference bank) this whole
//   abstraction exists for.
// - The Insights instrumentation bar:
//   ComputeStructure()/ProbeNovelty()/ComputeCorrespondence() each carry a named
//   TRACE_CPUPROFILER_EVENT_SCOPE_ON_CHANNEL on the shared SuperFAISS trace channel,
//   riding the runtime module's own instrumented query path underneath. Read-only and
//   non-perturbing by the same rule the runtime module states (SuperFAISSSubsystem.h)
//   -- provable, not just claimed: the determinism suite runs trace-OFF and trace-ON
//   and asserts one identical result.
class SSuperFAISSBankInspector : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSuperFAISSBankInspector) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	// View A (Structure), section 25.5: runs the chunked slow-task pass over the current
	// sample+scope (BuildKnnNeighbors -> MutualFilter -> BuildDuplicateGroups ->
	// ConnectedComponents, M1 graph.h) and populates StructureClusters /
	// StructureOutlierSampleIndices / StructureComponentIdBySampleIndex.
	void ComputeStructure();

	// View B (Novelty), section 25.5: parses `Text` exactly as RunQuery does (row id or
	// `#index`), self-excludes the probed row (V32-G7), and populates NoveltyResult +
	// NoveltyEvidenceLines. On a Cosine bank the probe is normalized to unit L2 norm at
	// this call site before reaching the limb-2 primitive —
	// never inside the core entry itself.
	void ProbeNovelty(const FString& Text);

	// View C (Correspondence), section 25.5: the second-bank slot (section 25.3
	// E-D1-1..4) + "Compute correspondence" trigger. Runs the SAME BuildAnalysisSample
	// construction View A/B's baseline uses for the A-side sample (respecting the shared
	// analysis scope), then MutualNearestMatches (matching.h, M3, sampled-A-verified-
	// against-full-banks) against a full-bank view of B and of A (also BuildAnalysisSample,
	// called with the bank's own live Count so the "sample" is the identity — every live
	// row, same channel-scope slicing, zero-copy only when the scope is the whole row).
	// Compatibility (dims, metric — Quantization may differ, disclosed) is checked BEFORE
	// any compute begins (the "second-bank compatibility rejection matrix", section 25.9
	// dim 2) — a failure there sets a line-item CorrespondenceStatus and clears
	// MatchPairResults to empty (the late-rejection UI contract, audit N-3), never leaving
	// a stale pair list from an earlier valid pair rendering beside the rejection. State
	// (matched/ambiguous/unmatched) is CALLER-composed here from each pair's raw
	// CslsMargin against Settings->CslsMarginThreshold (matching.h's own contract: no
	// verdict entry exists in MatchPair itself).
	void ComputeCorrespondence();

	// The "Open scratch archive..." affordance. Reads
	// Bytes as a scratch archive (core Load, reject-over-degrade -- "a bad blob leaves
	// the bank unchanged") into a transient, editor-private ScratchBank and, on success,
	// makes it this widget's PRIMARY inspection source (superseding the asset-registry
	// combo selection, mutually exclusive per slot) and invalidates the analysis caches
	// (the NEW archive-swap leg of the reset matrix, audit F3). On failure the existing
	// source (an asset, or a previously-open archive) is left EXACTLY as it was, and
	// GetArchiveOpenStatus() carries core Load's own rejection surfaced as a line-item
	// status. Synchronous by design (this round's own judgment call, per the
	// commissioning brief's invitation to make one): a one-shot byte-buffer read plus one
	// Load call is not a bank-wide O(n) pass the way Structure/Novelty/Correspondence
	// are, so it does not warrant SuperFAISSInspectorSlowTask's modal chunked treatment.
	bool OpenScratchArchiveFromBytes(const TArray<uint8>& Bytes, const FString& DisplayName);
	// The second-bank slot's mirror (temper W1: "archive-vs-baked correspondence...
	// exercised in slot A and in slot B separately" is explicitly in scope for 4b, not a
	// per-pane follow-on the way the live channel-weighted query pane is).
	bool OpenSecondScratchArchiveFromBytes(const TArray<uint8>& Bytes, const FString& DisplayName);
	const FString& GetArchiveOpenStatus() const { return ArchiveOpenStatus; }
	const FString& GetSecondArchiveOpenStatus() const { return SecondArchiveOpenStatus; }

	// The resolved current inspection source per slot (test + future UI binding
	// surface): Archive-kind when that slot's "Open scratch archive..." has succeeded
	// and no asset re-selection has superseded it since; Asset-kind from the existing
	// combo otherwise; None-kind if neither.
	FSuperFAISSInspectionSource GetPrimarySource() const;
	FSuperFAISSInspectionSource GetSecondSource() const;

	// Read-only accessors for View A/B state (test + future UI binding surface).
	const TArray<FSuperFAISSStructureCluster>& GetStructureClusters() const { return StructureClusters; }
	const TArray<int32>& GetStructureOutlierSampleIndices() const { return StructureOutlierSampleIndices; }
	const TArray<int32>& GetStructureComponentIdBySampleIndex() const { return StructureComponentIdBySampleIndex; }
	// Slot 4b test surface: sample position -> bank/archive SOURCE row index (already a
	// private member wired by both ComputeStructure() and ComputeProjection() -- this
	// exposes it read-only, no new state or logic, the same "test + future UI binding
	// surface" reasoning behind every other accessor in this block). Needed to check the
	// dim-7 absolute claim ("a deleted row can never score as live") against a PRUNED
	// archive's cluster membership: a component's members are SAMPLE positions, so
	// proving none of them resolve to a tombstoned SOURCE row needs this mapping.
	const TArray<int32>& GetStructureSampleSourceIndices() const { return StructureSampleSourceIndices; }
	const FString& GetStructureStatus() const { return StructureStatus; }
	const FSuperFAISSNoveltyResult& GetNoveltyResult() const { return NoveltyResult; }
	const TArray<TSharedPtr<FString>>& GetNoveltyEvidenceLines() const { return NoveltyEvidenceLines; }
	const FString& GetNoveltyStatus() const { return NoveltyStatus; }
	// The View B verdict readout's exact rendered copy (section 25.5): the same text the
	// panel's Text_Lambda displays, factored out so a test can assert the RENDERED text
	// rather than only the struct fields it is built from. Plan section 25.3's exclusion
	// disclosure ("Structure and Correspondence already disclose it") extends to Novelty
	// here: when NoveltyResult.ZeroEnergyExcludedCount is nonzero, the count is appended,
	// the same idiom StructureStatus/BuildNoveltyVerdictText's siblings already use.
	FString BuildNoveltyVerdictText() const;

	// Read-only accessors for View C state (test + future UI binding surface).
	const TArray<FSuperFAISSMatchPairResult>& GetMatchPairResults() const { return MatchPairResults; }
	const FString& GetCorrespondenceStatus() const { return CorrespondenceStatus; }

	// The View A determinism-tier disclosure copy (section 25.5), exposed so the panel
	// copy assertion (section 25.9 dim 7 doc cell) does not hand-duplicate the string.
	static const TCHAR* StructureDisclosureCopy();
	// The View B Dot-bank verdict-unavailable status text, verbatim (section 25.5).
	static const TCHAR* DotUnavailableStatus();

#if WITH_DEV_AUTOMATION_TESTS
	// Test seam: assigns a bank directly, bypassing the asset-registry enumeration
	// RefreshBankList() drives in a real editor session (no automation test registers
	// a saved asset), then runs the widget's own OnBankSelected() reset — an
	// in-memory NewObject<USuperFAISSVectorBank>()::InitFromSource() bank behaves
	// identically to a registry-discovered one from this point on.
	void SetBankForTest(USuperFAISSVectorBank* Bank);
	// Test seam (slot 4): assigns the SECOND bank directly, the same bypass
	// SetBankForTest gives the primary — mirrors OnSecondBankSelected()'s reset.
	void SetSecondBankForTest(USuperFAISSVectorBank* Bank);
	// Test seam: selects the projection/analysis scope by name ("(whole row)" or a
	// channel name), mirroring the combo box's OnSelectionChanged handler.
	void SetAnalysisScopeForTest(const FString& ScopeName);
	// Test seam (section 25.9 dim 3/5 cancel-path cells): when >= 0, the NEXT chunked
	// pass's cancel poll reports "cancel" once this many chunk boundaries have been
	// crossed. -1 (default) never cancels. Production cancellation reads the real
	// modal slow task; this is the only way an automation test drives cancellation
	// deterministically without a live user (no other seam is named in the plan for
	// this — the same forced-reading shape as the RunChunked contract itself).
	int32 DebugCancelAfterChunks = -1;
	// Test-visible chunk counters (section 25.9 dim 3/5): incremented once per chunk
	// boundary ComputeStructure() / ProbeNovelty()'s baseline-calibration leg / (slot 4)
	// ComputeCorrespondence() actually reaches, independent of whether the chunk's real
	// work is implemented yet.
	int32 StructureChunksProcessedForTest = 0;
	int32 NoveltyBaselineChunksProcessedForTest = 0;
	int32 CorrespondenceChunksProcessedForTest = 0;

	// Test seam (slot 4b): direct access to the source-generalized sample construction --
	// the crux space-law mechanism -- independent of whether every public Compute*
	// trigger has been repointed to read an archive source yet (ComputeStructure() and
	// ComputeCorrespondence() are, this round; ProbeNovelty() is explicitly routed
	// onward -- see the test-design artifact's scope section). Mirrors the production
	// signature exactly; a thin pass-through, the same shape as SetBankForTest et al.
	bool BuildAnalysisSampleForTest(const FSuperFAISSInspectionSource& Source, int32 SampleLimit,
		TArray<uint8, TAlignedHeapAllocator<16>>& OutPayload, TArray<float>& OutScales,
		superfaiss::BankView& OutView, TArray<int32>& OutSourceIndices,
		bool bSkipTombstonedRows = true, int32* OutZeroEnergyExcludedCount = nullptr,
		TArray<uint32>* OutZeroEnergyExcludeBits = nullptr) const
	{
		return BuildAnalysisSample(Source, SampleLimit, OutPayload, OutScales, OutView, OutSourceIndices,
			bSkipTombstonedRows, OutZeroEnergyExcludedCount, OutZeroEnergyExcludeBits);
	}
#endif

private:
	void RefreshBankList();
	USuperFAISSVectorBank* GetSelectedBank() const;
	void OnBankSelected();
	void RunQuery(const FString& Text);
	void ComputeProjection();
	FString BankInfoLine() const;

	// View C (Correspondence), section 25.3 E-D1-1..4: the second bank slot — a second
	// SComboBox over the SAME asset-registry list as the primary, held as
	// TWeakObjectPtr, resolved at compute time (the primary's own pattern; E-D1-1). Not
	// asset state, not settings state — transient widget state (E-D1-2), reset by
	// OnSecondBankSelected() exactly as the primary resets by OnBankSelected() (the
	// V32-G2 discipline extended to the pair, E-D1-3's "primary swap while a pair is
	// loaded resets correspondence state").
	void RefreshSecondBankList();
	USuperFAISSVectorBank* GetSelectedSecondBank() const;
	// A second-bank change is a leg of the section 25.9 dim 1 reset matrix
	// ("second-bank change") — calls InvalidateAnalysisCaches(), exactly OnBankSelected()'s
	// own call, extending the V32-G2 discipline to the pair.
	void OnSecondBankSelected();

	// View C: the second-bank compatibility rejection matrix (section 25.9 dim 2) —
	// Dims and Metric must match (E-D1-3); Quantization may differ (disclosed, not
	// rejected). Returns true and leaves OutReason untouched iff A/B are compatible;
	// returns false and sets OutReason to the exact line-item status text otherwise.
	// A null B (no second bank selected) and a B that fails IsValid() are their own
	// named reasons — the "no second bank selected" / "second bank: invalid asset"
	// idioms extending RunQuery's/ComputeStructure's own "no valid bank selected"
	// idiom to the second-bank slot (a forced reading: section 25.9's rejection matrix
	// names "invalid asset" but not this exact split; the design choice mirrors how
	// section 25.5 already distinguishes "no bank selected" (RunQuery/ComputeStructure)
	// from a selected-but-invalid one elsewhere in this class).
	bool CheckSecondBankCompatible(const USuperFAISSVectorBank& A, const USuperFAISSVectorBank* B,
		FString& OutReason) const;

	// Slot 4b: the SAME compatibility rejection matrix, generalized to either inspection
	// source (real, mechanical -- the same dims/metric field comparisons via
	// FSuperFAISSInspectionSource's own accessors, so it fires identically for an
	// asset-vs-asset, asset-vs-archive, or archive-vs-archive pair). This is the overload
	// ComputeCorrespondence() calls now; the asset-only overload above is left in place,
	// unused by any call site, so slot 3/4's own closed-green behavior is never disturbed
	// by this round's edit.
	bool CheckSecondBankCompatible(const FSuperFAISSInspectionSource& A,
		const FSuperFAISSInspectionSource& B, FString& OutReason) const;

	// Scatter hover tooltip: the real bank id (or #index) of the sample position under the
	// cursor, plus its cluster membership if Structure has been computed (its component id
	// + size, or "Outlier", or nothing if Structure hasn't run yet). Passed into
	// SSuperFAISSScatter as a lambda so the scatter widget stays a dumb painter — it does
	// not know about bank ids or clusters, only sample-position geometry.
	FText GetScatterPointLabel(int32 SampleIndex) const;
	// Shared by GetScatterPointLabel and OnStructureItemSelected: the StructureClusters
	// entry with the given canonical ComponentId, or nullptr (an outlier/no-component id).
	const FSuperFAISSStructureCluster* FindStructureCluster(int32 ComponentId) const;

	// UI-only (not analysis state): rebuilds StructureListRoots from StructureClusters/
	// StructureOutlierSampleIndices as a two-level tree (one header per cluster + an
	// Outliers header, each with one child row per member, labeled by the member's actual
	// bank id via StructureSampleSourceIndices — section 25.5 "members by Id ... else
	// #index"), and clears the stale selection/highlight. Called after ComputeStructure()
	// and whenever the list would otherwise go stale (scope change, bank re-select).
	void RebuildStructureClusterList();
	// section 25.5: selecting a cluster/Outliers HEADER highlights all its points;
	// selecting a single MEMBER row highlights that row plus its k nearest (from the
	// persisted StructureNeighborSampleIndices, section 25.5 "selecting a row highlights
	// its component + its k nearest").
	void OnStructureItemSelected(TSharedPtr<FSuperFAISSStructureListItem> Item);
	void GetStructureItemChildren(TSharedPtr<FSuperFAISSStructureListItem> Item,
		TArray<TSharedPtr<FSuperFAISSStructureListItem>>& OutChildren) const
	{
		if (Item.IsValid())
		{
			OutChildren = Item->Children;
		}
	}

	// Section 25.3's coarse, one-rule invalidation (audit N-2, "the whole analysis cache
	// (structure + baseline + matches), one rule for every panel" — matches names
	// Correspondence explicitly): clears the Structure, Novelty, AND (slot 4)
	// Correspondence caches together — called by OnBankSelected(), by an analysis-scope
	// change, and (slot 4, the NEW leg) by OnSecondBankSelected(). NOT called on a
	// settings (analysis-parameter) change: no widget event exists to fire it from
	// (UDeveloperSettings has no per-instance change notification this class subscribes
	// to). Structure never caches across calls (ComputeStructure recomputes fully every
	// call, reading Settings fresh), so it has no staleness exposure; Correspondence is
	// built the SAME way (ComputeCorrespondence recomputes fully every call, reading
	// MatchK/CslsMarginThreshold fresh — no persistent workspace the way the Novelty
	// baseline has), so a MatchK/CslsMarginThreshold edit is likewise not a staleness
	// hazard: the next "Compute correspondence" click simply reads the new values, with
	// nothing stale left to invalidate (mirrors Structure's reasoning exactly, NOT the
	// Novelty baseline's self-check reasoning below). The Novelty baseline DOES cache
	// across calls — its own staleness protection is a structural self-check
	// (NoveltyBaselineK/SampleLimit vs current settings) inside ProbeNovelty itself, not
	// a trigger through here.
	void InvalidateAnalysisCaches();

	// Section 25.3's "one shared cap" + "the projection scope combo IS the one
	// analysis-scope selector every sample-scoped pass reads": the deterministic
	// sample-and-scope construction View A (Structure) and View B's baseline
	// calibration both run on. Endpoint-inclusive even sampling — always exactly
	// min(Bank.Count, SampleLimit) rows, always including source row 0 and the
	// bank's last row (distinct from ComputeProjection's own pre-existing stride
	// formula above, which is untouched — out of slot 3's scope). Returns false on
	// an unknown channel scope name (stale combo state). `OutSourceIndices[s]` is
	// the bank source row that sample position `s` was drawn from.
	//
	// On a channel-scoped Cosine bank, every sliced row is
	// renormalized to unit norm OVER THE SLICE before being written to OutPayload -- a
	// channel slice of a whole-row-unit row is not itself unit-norm, and this view's
	// plain Cosine kernel (channels=nullptr) assumes the unit precondition. Int8: the
	// per-row OutScales entry is rescaled by the slice's own inverse norm (the DAZ-safe
	// decode bake.cpp's ComputeChannelInverseNorms uses), so the stored int8 bytes are
	// untouched and only the scale carries the renormalization. Float32: the copied
	// slice's elements are scaled in place. A row with zero energy in the scoped channel
	// has no direction to renormalize to; OutZeroEnergyExcludedCount, if non-null,
	// receives the count so the caller can disclose it. Whole-row scope, L2, and Dot are
	// untouched -- this applies to the Cosine metric on a channel scope only.
	//
	// bCompactZeroEnergy (Finding 6, a regression review caught in Finding 1's own fix)
	// selects how a zero-energy row is handled, and MUST match the caller's own
	// index-identity requirement:
	//   - true (the default; every SAMPLE build -- Structure, Projection, Correspondence's
	//     A-side sample, the Novelty baseline): the row is DROPPED from the sample.
	//     OutSourceIndices is the mapping and carries no index-identity requirement, so
	//     compaction is correct and cheapest here.
	//   - false (a FULL-VIEW IDENTITY build ONLY -- SampleLimit == the source's own
	//     published count, called by the Source overload when bSkipTombstonedRows=false):
	//     the row is KEPT at its native position (unrenormalized -- there is no direction
	//     to renormalize a zero-energy slice to, and its payload is never scored), and its
	//     bit is set in OutZeroEnergyExcludeBits instead. This preserves the SAME
	//     index-identity the tombstone space law already requires of a full-view build, so
	//     the caller ORs both bit arrays into one excludeBits set aligned to the same
	//     native index space.
	// OutZeroEnergyExcludeBits, when bCompactZeroEnergy is false and non-null, is sized to
	// ceil(sample count / 32) words (the same shape as GetTombstoneWords()) and zeroed;
	// ignored when bCompactZeroEnergy is true.
	bool BuildAnalysisSample(const USuperFAISSVectorBank& Bank, int32 SampleLimit,
		TArray<uint8, TAlignedHeapAllocator<16>>& OutPayload, TArray<float>& OutScales,
		superfaiss::BankView& OutView, TArray<int32>& OutSourceIndices,
		int32* OutZeroEnergyExcludedCount = nullptr, bool bCompactZeroEnergy = true,
		TArray<uint32>* OutZeroEnergyExcludeBits = nullptr) const;

	// The abstraction's own crux: the
	// SAME endpoint-inclusive even-sampling construction, generalized to either
	// inspection source. For an Asset-kind source this delegates unchanged to the
	// overload above (bit-identical, no behavior change on the already-proven asset
	// path). For an Archive-kind source: channel-scoped archive analysis is a real,
	// disclosed, defined rejection (returns false when a non-"(whole row)" scope is
	// selected -- routed onward alongside the live channel-weighted query pane's own
	// identical asset-only posture, section 25.3; a genuine unit-space obstacle, not a
	// preference).
	//
	// bSkipTombstonedRows selects WHICH of section 25.3's two space-law placements this
	// call is for -- they are mutually exclusive, never both applied to the same view:
	//   - true (the default; every sample-scoped pass -- Structure's/Correspondence's
	//     A-side sample): construction-time discharge. The ascending list of LIVE source
	//     indices is built first (tombstoned rows dropped via Source.GetTombstoneWords()
	//     before striding), then this sampling formula runs over that live-row list
	//     instead of the raw published range. A fully-tombstoned source (no live rows at
	//     all) returns false -- nothing to sample, audit F4's live-0 count class.
	//   - false (Correspondence's full-B/full-A views ONLY): the raw published range,
	//     identity-mapped, tombstoned rows INCLUDED -- because the caller is about to OR
	//     Source.GetTombstoneWords() into the kernel's own excludeBits as a SEPARATE,
	//     source-space-aligned runtime exclusion (section 25.3's other placement, "the
	//     full-view... carries the OR, in source space"). Compacting the view here would
	//     desynchronize it from those bits (they'd land on the wrong now-shifted rows) --
	//     exactly the space-law hazard the plan's own sentence warns about ("an
	//     excludeBits bitset is always indexed in the index space of the view it
	//     accompanies").
	// OutZeroEnergyExcludedCount / OutZeroEnergyExcludeBits: the Asset-kind overload's own
	// disclosure, forwarded unchanged, with bCompactZeroEnergy DERIVED from
	// bSkipTombstonedRows -- the two flags select the SAME axis (sample vs. full-view
	// identity) by construction, since every call site that passes bSkipTombstonedRows as
	// one of the two literal values also means it as the other. The
	// Archive-kind path rejects a channel scope outright, so it never has a zero-energy
	// row to exclude and leaves both outputs at 0/empty.
	bool BuildAnalysisSample(const FSuperFAISSInspectionSource& Source, int32 SampleLimit,
		TArray<uint8, TAlignedHeapAllocator<16>>& OutPayload, TArray<float>& OutScales,
		superfaiss::BankView& OutView, TArray<int32>& OutSourceIndices,
		bool bSkipTombstonedRows = true, int32* OutZeroEnergyExcludedCount = nullptr,
		TArray<uint32>* OutZeroEnergyExcludeBits = nullptr) const;

	TArray<TSharedPtr<FString>> BankNames;
	TArray<TWeakObjectPtr<USuperFAISSVectorBank>> BankAssets;
	TSharedPtr<FString> SelectedBankName;

	// Slot 4b: the primary slot's open archive, if any -- mutually exclusive with
	// SelectedBankName above (OnBankSelected()/OpenScratchArchiveFromBytes() each clear
	// the other). An OWNED reference (see FSuperFAISSInspectionSource's own class
	// comment for why this is a TStrongObjectPtr, not a TWeakObjectPtr like BankAssets).
	TStrongObjectPtr<USuperFAISSScratchBank> PrimaryArchiveBank;
	FString PrimaryArchiveDisplayName;
	// The "Open scratch archive..." affordance's own status line (distinct from
	// StructureStatus/NoveltyStatus/CorrespondenceStatus -- this reports the OPEN action
	// itself, section 25.9's archive rejection matrix), and its second-slot mirror.
	FString ArchiveOpenStatus;
	FString SecondArchiveOpenStatus;

	// View C (Correspondence) second-bank slot (section 25.3 E-D1-1): the SAME
	// asset-registry-enumeration pattern as the primary picker, held separately —
	// transient widget state (E-D1-2), never persisted.
	TArray<TSharedPtr<FString>> SecondBankNames;
	TArray<TWeakObjectPtr<USuperFAISSVectorBank>> SecondBankAssets;
	TSharedPtr<FString> SelectedSecondBankName;
	// Slot 4b, second-bank mirror (temper W1): the same archive/asset mutual exclusion as
	// the primary slot.
	TStrongObjectPtr<USuperFAISSScratchBank> SecondArchiveBank;
	FString SecondArchiveDisplayName;

	TArray<TSharedPtr<FString>> ResultLines;
	TSharedPtr<class SListView<TSharedPtr<FString>>> ResultList;

	// Channel query state: one weight per channel of the selected bank, slider-driven.
	TArray<TSharedPtr<FString>> ChannelSliderNames; // parallel to ChannelWeights
	TArray<float> ChannelWeights;
	TSharedPtr<class SVerticalBox> ChannelSliderBox;
	void RebuildChannelSliders();

	// Projection state: sampled 2D coords, normalized into [0,1] for the paint pass.
	TArray<FVector2f> ProjectedPoints;
	FString ProjectionStatus;

	// Projection / analysis scope: "(whole row)" + the selected bank's channel names.
	// Shared by the projection AND by View A/B (see the class-header design note).
	TArray<TSharedPtr<FString>> ProjectionScopes;
	TSharedPtr<FString> SelectedProjectionScope;

	// View A (Structure) state: one shared analysis cache with Novelty's baseline
	// (section 25.9 dim 1, audit N-2), reset by OnBankSelected / scope change /
	// parameter change exactly like ProjectedPoints (the V32-G2 discipline extended,
	// section 25.3's "Cache lifetime is per-widget-session" rule).
	TArray<FSuperFAISSStructureCluster> StructureClusters;
	TArray<int32> StructureOutlierSampleIndices;
	// Parallel to ProjectedPoints; -1 for an outlier sample row. The 1:1 scatter join.
	TArray<int32> StructureComponentIdBySampleIndex;
	FString StructureStatus;
	// Persisted analysis byproducts (section 25.5 "members by Id" + "its k nearest", plus
	// the scatter hover tooltip), kept past ComputeStructure()'s/ComputeProjection()'s own
	// return so the UI can resolve a sample position's real bank id and neighbors without
	// re-running the pipeline. Sample-position-indexed, same space as
	// StructureComponentIdBySampleIndex/ProjectedPoints. StructureSampleSourceIndices is
	// written by BOTH ComputeStructure() and ComputeProjection() (they share one
	// BuildAnalysisSample construction, F2 fix) so hover labels work even before Compute
	// structure has been run; StructureNeighborSampleIndices only by ComputeStructure()
	// (no neighbor list exists until the k-NN pass has run).
	TArray<int32> StructureSampleSourceIndices; // sample position -> bank row index
	TArray<int32> StructureNeighborSampleIndices; // View.count * StructureNeighborK, -1 = none
	int32 StructureNeighborK = 0;
	// UI-only: the highlighted sample indices, each tagged with WHY it's highlighted
	// (section 25.5 "selecting a cluster highlights its points" / "selecting a row
	// highlights its component + its k nearest") — a cluster-header select tags every
	// member Component; a row select tags its component Component, its neighbors Neighbor,
	// and the row itself Selected (in that increasing-priority order, so a sample that
	// qualifies for more than one keeps the more specific tag). Not analysis state —
	// cleared alongside the list whenever it goes stale (ComputeStructure re-running is the
	// only source of truth for what a valid selection would even mean).
	TMap<int32, ESuperFAISSStructureHighlight> HighlightedSampleIndices;
	TSharedPtr<class STreeView<TSharedPtr<FSuperFAISSStructureListItem>>> StructureClusterTree;
	TArray<TSharedPtr<FSuperFAISSStructureListItem>> StructureListRoots; // one per cluster + Outliers
	// UI-only: the evidence list's display strings for View B (Novelty), rebuilt each probe.
	TSharedPtr<class SListView<TSharedPtr<FString>>> NoveltyEvidenceList;

	// View B (Novelty) state.
	FSuperFAISSNoveltyResult NoveltyResult;
	TArray<TSharedPtr<FString>> NoveltyEvidenceLines;
	FString NoveltyStatus;
	// The novelty baseline calibration (section 25.5 "on first probe per bank the
	// baseline calibrates"): cached per analysis cache lifetime, same invalidation
	// triggers as StructureClusters (bank re-select, scope change) — PLUS a structural
	// self-check against the settings it was calibrated with: a bank-select/scope-change
	// event is not the only way the baseline can go stale — a NoveltyK or SampleLimit
	// settings edit between probes is not a widget event this class can observe at all
	// (UDeveloperSettings has no per-instance change notification this widget subscribes
	// to), so recalibration is triggered by comparing the CACHED K/SampleLimit against the
	// CURRENT settings on every probe, not by a fired invalidation event — the cache cannot
	// be stale-and-unnoticed by construction, never a vigilance rule to remember to wire.
	TArray<float> NoveltyBaselineSortedDistances;
	bool bNoveltyBaselineCalibrated = false;
	int32 NoveltyBaselineK = 0;
	int32 NoveltyBaselineSampleLimit = 0;

	// View C (Correspondence) state (section 25.5): the matched-pair list, one entry per
	// CHECKED A sample row (matching.h's outPairs contract — every sample row, matched
	// or not). Never cached across ComputeCorrespondence() calls (see
	// InvalidateAnalysisCaches()'s comment) — rebuilt fully on every trigger click, same
	// posture as StructureClusters, not the Novelty-baseline caching posture.
	TArray<FSuperFAISSMatchPairResult> MatchPairResults;
	FString CorrespondenceStatus;
	// UI-only: MatchPairResults formatted for the list view (source id/index, partner
	// id/index or "unmatched", margin, state column — TEXT ONLY, never color-coded,
	// section 25.5). Rebuilt alongside MatchPairResults; not analysis state.
	TArray<TSharedPtr<FString>> MatchPairDisplayLines;
	TSharedPtr<class SListView<TSharedPtr<FString>>> MatchPairList;

	static constexpr int32 PcaIterations = 24;
	// Section 25.5: the verdict carries a low-confidence marking below this effective
	// sample size (the statistical floor the deleted N_cal embodied, relocated as
	// disclosure — section 25.9 dim 7).
	static constexpr int32 NoveltyLowConfidenceFloor = 64;
};
