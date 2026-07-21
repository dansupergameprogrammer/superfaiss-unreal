#include "SuperFAISSInspectorSettings.h"

// Section 25.9 dim 2 (audit G-1): the settings object is a trust boundary — every field is
// clamped to its documented range at read, never left to feed array sizing unchecked.
// Documented ranges: SampleLimit in [1, kHardSampleCap]; NoveltyLambda in [0, 1]; every
// k-shaped field (StructureK, MinComponentSize, NoveltyK) floored at 1, no documented
// ceiling.

int32 USuperFAISSInspectorSettings::GetClampedSampleLimit() const
{
	return FMath::Clamp(SampleLimit, 1, kHardSampleCap);
}

int32 USuperFAISSInspectorSettings::GetClampedStructureK() const
{
	return FMath::Max(StructureK, 1);
}

int32 USuperFAISSInspectorSettings::GetClampedMinComponentSize() const
{
	return FMath::Max(MinComponentSize, 1);
}

int32 USuperFAISSInspectorSettings::GetClampedNoveltyK() const
{
	return FMath::Max(NoveltyK, 1);
}

float USuperFAISSInspectorSettings::GetClampedNoveltyLambda() const
{
	return FMath::Clamp(NoveltyLambda, 0.0f, 1.0f);
}

int32 USuperFAISSInspectorSettings::GetClampedMatchK() const
{
	return FMath::Max(MatchK, 1);
}

int32 USuperFAISSInspectorSettings::EffectiveDefaultSample(int32 Dims, int64 BudgetMacs) const
{
	const int32 ClampedLimit = GetClampedSampleLimit();
	if (Dims < 1)
	{
		return ClampedLimit;
	}
	const int32 BudgetSample = FMath::FloorToInt(
		FMath::Sqrt(static_cast<double>(BudgetMacs) / static_cast<double>(Dims)));
	return FMath::Min(ClampedLimit, BudgetSample);
}
