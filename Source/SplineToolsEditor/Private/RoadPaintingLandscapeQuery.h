#pragma once

#include "CoreMinimal.h"

class AActor;
class UWorld;

namespace RoadPaintingLandscapeQuery
{
	bool FindSurfaceAlongSegment(
		UWorld* World,
		const FVector& Start,
		const FVector& End,
		FVector& OutLocation,
		float& OutDistance,
		const AActor* IgnoredActor = nullptr);
}
