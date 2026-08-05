#include "RoadPaintingLandscapeQuery.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "LandscapeComponent.h"
#include "LandscapeHeightfieldCollisionComponent.h"
#include "LandscapeProxy.h"

namespace
{
	constexpr int32 HeightfieldSearchSteps = 8;
	constexpr int32 HeightfieldRefinementSteps = 16;

	bool IsLandscapeHit(const FHitResult& Hit)
	{
		if (IsValid(Hit.GetActor()) && Hit.GetActor()->IsA<ALandscapeProxy>())
		{
			return true;
		}

		return IsValid(Hit.GetComponent())
			&& (Hit.GetComponent()->IsA<ULandscapeComponent>()
				|| Hit.GetComponent()->IsA<ULandscapeHeightfieldCollisionComponent>());
	}

	bool FindLandscapeCollisionHit(
		UWorld* World,
		const FVector& Start,
		const FVector& End,
		bool bTraceComplex,
		bool bUseObjectQuery,
		FVector& OutLocation,
		float& OutDistance,
		const AActor* IgnoredActor)
	{
		TArray<FHitResult> Hits;
		FCollisionQueryParams QueryParams(
			SCENE_QUERY_STAT(RoadPaintingLandscape),
			bTraceComplex,
			IgnoredActor);
		if (bUseObjectQuery)
		{
			FCollisionObjectQueryParams ObjectQueryParams;
			ObjectQueryParams.AddObjectTypesToQuery(ECC_WorldStatic);
			World->LineTraceMultiByObjectType(
				Hits,
				Start,
				End,
				ObjectQueryParams,
				QueryParams);
		}
		else
		{
			World->LineTraceMultiByChannel(
				Hits,
				Start,
				End,
				ECC_Visibility,
				QueryParams);
		}

		for (const FHitResult& Hit : Hits)
		{
			if (IsLandscapeHit(Hit))
			{
				OutLocation = Hit.ImpactPoint;
				OutDistance = FVector::Distance(Start, Hit.ImpactPoint);
				return true;
			}
		}
		return false;
	}

	bool IntersectAxis(
		float Origin,
		float Direction,
		float Minimum,
		float Maximum,
		float& InOutMinimumDistance,
		float& InOutMaximumDistance)
	{
		if (FMath::IsNearlyZero(Direction))
		{
			return Origin >= Minimum && Origin <= Maximum;
		}

		float FirstDistance = (Minimum - Origin) / Direction;
		float SecondDistance = (Maximum - Origin) / Direction;
		if (FirstDistance > SecondDistance)
		{
			Swap(FirstDistance, SecondDistance);
		}
		InOutMinimumDistance = FMath::Max(InOutMinimumDistance, FirstDistance);
		InOutMaximumDistance = FMath::Min(InOutMaximumDistance, SecondDistance);
		return InOutMinimumDistance <= InOutMaximumDistance;
	}

	TOptional<float> GetLandscapeHeight(
		const ALandscapeProxy* Landscape,
		const FVector& Location)
	{
		TOptional<float> Height = Landscape->GetHeightAtLocation(
			Location,
			EHeightfieldSource::Editor);
		if (!Height.IsSet())
		{
			Height = Landscape->GetHeightAtLocation(
				Location,
				EHeightfieldSource::Complex);
		}
		if (!Height.IsSet())
		{
			Height = Landscape->GetHeightAtLocation(
				Location,
				EHeightfieldSource::Simple);
		}
		return Height;
	}

	bool FindHeightfieldHit(
		const ALandscapeProxy* Landscape,
		const FVector& Start,
		const FVector& Direction,
		float SegmentLength,
		FVector& OutLocation,
		float& OutDistance)
	{
		const FBox Bounds = Landscape->GetComponentsBoundingBox(true);
		if (!Bounds.IsValid)
		{
			return false;
		}

		float MinimumDistance = 0.0f;
		float MaximumDistance = SegmentLength;
		if (!IntersectAxis(
				Start.X,
				Direction.X,
				Bounds.Min.X,
				Bounds.Max.X,
				MinimumDistance,
				MaximumDistance)
			|| !IntersectAxis(
				Start.Y,
				Direction.Y,
				Bounds.Min.Y,
				Bounds.Max.Y,
				MinimumDistance,
				MaximumDistance))
		{
			return false;
		}

		MinimumDistance = FMath::Clamp(MinimumDistance, 0.0f, SegmentLength);
		MaximumDistance = FMath::Clamp(MaximumDistance, 0.0f, SegmentLength);
		float PreviousDistance = MinimumDistance;
		float PreviousDifference = 0.0f;
		TOptional<float> PreviousHeight;
		for (int32 StepIndex = 0; StepIndex <= HeightfieldSearchSteps; ++StepIndex)
		{
			const float CurrentDistance = FMath::Lerp(
				MinimumDistance,
				MaximumDistance,
				static_cast<float>(StepIndex) / HeightfieldSearchSteps);
			FVector CurrentLocation = Start + Direction * CurrentDistance;
			TOptional<float> CurrentHeight = GetLandscapeHeight(Landscape, CurrentLocation);
			if (!CurrentHeight.IsSet())
			{
				PreviousHeight.Reset();
				continue;
			}

			const float CurrentDifference = CurrentLocation.Z - CurrentHeight.GetValue();
			if (PreviousHeight.IsSet()
				&& (FMath::IsNearlyZero(PreviousDifference)
					|| FMath::IsNearlyZero(CurrentDifference)
					|| FMath::Sign(PreviousDifference) != FMath::Sign(CurrentDifference)))
			{
				float LowerDistance = PreviousDistance;
				float UpperDistance = CurrentDistance;
				float LowerDifference = PreviousDifference;
				for (int32 RefinementIndex = 0;
					RefinementIndex < HeightfieldRefinementSteps;
					++RefinementIndex)
				{
					const float MiddleDistance = (LowerDistance + UpperDistance) * 0.5f;
					FVector MiddleLocation = Start + Direction * MiddleDistance;
					TOptional<float> MiddleHeight = GetLandscapeHeight(Landscape, MiddleLocation);
					if (!MiddleHeight.IsSet())
					{
						break;
					}
					const float MiddleDifference = MiddleLocation.Z - MiddleHeight.GetValue();
					if (FMath::Sign(LowerDifference) == FMath::Sign(MiddleDifference))
					{
						LowerDistance = MiddleDistance;
						LowerDifference = MiddleDifference;
					}
					else
					{
						UpperDistance = MiddleDistance;
					}
				}

				OutDistance = (LowerDistance + UpperDistance) * 0.5f;
				OutLocation = Start + Direction * OutDistance;
				TOptional<float> FinalHeight = GetLandscapeHeight(Landscape, OutLocation);
				if (FinalHeight.IsSet())
				{
					OutLocation.Z = FinalHeight.GetValue();
					return true;
				}
			}

			PreviousDistance = CurrentDistance;
			PreviousHeight = CurrentHeight;
			PreviousDifference = CurrentDifference;
		}
		return false;
	}

	bool FindLandscapeHeightfieldHit(
		UWorld* World,
		const FVector& Start,
		const FVector& End,
		FVector& OutLocation,
		float& OutDistance)
	{
		const float SegmentLength = FVector::Distance(Start, End);
		if (SegmentLength <= KINDA_SMALL_NUMBER)
		{
			return false;
		}

		const FVector Direction = (End - Start) / SegmentLength;
		bool bFoundHit = false;
		float NearestDistance = SegmentLength;
		for (TActorIterator<ALandscapeProxy> Iterator(World); Iterator; ++Iterator)
		{
			FVector CandidateLocation;
			float CandidateDistance = 0.0f;
			if (FindHeightfieldHit(
					*Iterator,
					Start,
					Direction,
					SegmentLength,
					CandidateLocation,
					CandidateDistance)
				&& CandidateDistance < NearestDistance)
			{
				bFoundHit = true;
				NearestDistance = CandidateDistance;
				OutLocation = CandidateLocation;
			}
		}

		OutDistance = NearestDistance;
		return bFoundHit;
	}
}

bool RoadPaintingLandscapeQuery::FindSurfaceAlongSegment(
	UWorld* World,
	const FVector& Start,
	const FVector& End,
	FVector& OutLocation,
	float& OutDistance,
	const AActor* IgnoredActor)
{
	if (!World)
	{
		return false;
	}

	// Collision is fastest, while the heightfield fallback also covers editor cells
	// whose collision response or physics state is temporarily unavailable.
	if (FindLandscapeCollisionHit(
			World,
			Start,
			End,
			true,
			false,
			OutLocation,
			OutDistance,
			IgnoredActor)
		|| FindLandscapeCollisionHit(
			World,
			Start,
			End,
			false,
			false,
			OutLocation,
			OutDistance,
			IgnoredActor)
		|| FindLandscapeCollisionHit(
			World,
			Start,
			End,
			false,
			true,
			OutLocation,
			OutDistance,
			IgnoredActor))
	{
		return true;
	}

	return FindLandscapeHeightfieldHit(
		World,
		Start,
		End,
		OutLocation,
		OutDistance);
}
