#pragma once

#include "CoreMinimal.h"

/** Distances are centimetres along the authored spline; positions follow the road surface. */
struct FRoadNoPassingSample
{
	float Distance = 0.0f;
	FVector Position = FVector::ZeroVector;
};

struct FRoadNoPassingSettings
{
	float AnalysisDistance = 2000.0f;
	float BendThresholdDegrees = 12.0f;
	float CrestThresholdDegrees = 4.0f;
	float AdvanceDistance = 2000.0f;
	float MinimumLength = 2000.0f;
};

struct FRoadNoPassingRange
{
	float Start = 0.0f;
	float End = 0.0f;
	/** +1 is spline right (increasing-distance traffic); -1 is spline left. */
	int32 SideSign = 1;
};

namespace RoadNoPassing
{
	/** Pure geometric analysis. Samples must be ordered, with distinct distances. */
	SPLINETOOLS_API TArray<FRoadNoPassingRange> Analyze(const TArray<FRoadNoPassingSample>& Samples, const FRoadNoPassingSettings& Settings);
}
