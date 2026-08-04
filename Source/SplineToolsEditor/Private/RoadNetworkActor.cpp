#include "RoadNetworkActor.h"

#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "LandscapeProxy.h"
#include "ProceduralRoadJunctionActor.h"
#include "ScopedTransaction.h"

DEFINE_LOG_CATEGORY_STATIC(LogRoadPainting, Log, All);

namespace
{
	float DistanceSquaredToSegment2D(
		const FVector& Point,
		const FVector& Start,
		const FVector& End,
		float& OutAlpha)
	{
		const FVector2D Segment(End.X - Start.X, End.Y - Start.Y);
		const FVector2D Offset(Point.X - Start.X, Point.Y - Start.Y);
		const float SegmentLengthSquared = Segment.SizeSquared();
		OutAlpha = SegmentLengthSquared > KINDA_SMALL_NUMBER
			? FMath::Clamp(FVector2D::DotProduct(Offset, Segment) / SegmentLengthSquared, 0.0f, 1.0f)
			: 0.0f;
		const FVector2D Closest(Start.X + Segment.X * OutAlpha, Start.Y + Segment.Y * OutAlpha);
		return FVector2D::DistSquared(FVector2D(Point.X, Point.Y), Closest);
	}

	bool IntersectSegments2D(
		const FVector& FirstStart,
		const FVector& FirstEnd,
		const FVector& SecondStart,
		const FVector& SecondEnd,
		float& OutFirstAlpha,
		float& OutSecondAlpha)
	{
		const FVector2D FirstDirection(FirstEnd.X - FirstStart.X, FirstEnd.Y - FirstStart.Y);
		const FVector2D SecondDirection(SecondEnd.X - SecondStart.X, SecondEnd.Y - SecondStart.Y);
		const float Cross = FirstDirection.X * SecondDirection.Y
			- FirstDirection.Y * SecondDirection.X;
		if (FMath::Abs(Cross) <= KINDA_SMALL_NUMBER)
		{
			return false;
		}

		const FVector2D Offset(SecondStart.X - FirstStart.X, SecondStart.Y - FirstStart.Y);
		OutFirstAlpha = (Offset.X * SecondDirection.Y - Offset.Y * SecondDirection.X) / Cross;
		OutSecondAlpha = (Offset.X * FirstDirection.Y - Offset.Y * FirstDirection.X) / Cross;
		return OutFirstAlpha >= 0.0f && OutFirstAlpha <= 1.0f
			&& OutSecondAlpha >= 0.0f && OutSecondAlpha <= 1.0f;
	}

	bool LinkConnectsPoint(const FRoadNetworkLink& Link, const FGuid& PointId)
	{
		return Link.StartPointId == PointId || Link.EndPointId == PointId;
	}

	FGuid GetOtherPoint(const FRoadNetworkLink& Link, const FGuid& PointId)
	{
		return Link.StartPointId == PointId ? Link.EndPointId : Link.StartPointId;
	}

	FVector EvaluateHermiteDerivative(
		const FVector& Start,
		const FVector& StartTangent,
		const FVector& End,
		const FVector& EndTangent,
		float Alpha)
	{
		const float AlphaSquared = Alpha * Alpha;
		return Start * (6.0f * AlphaSquared - 6.0f * Alpha)
			+ StartTangent * (3.0f * AlphaSquared - 4.0f * Alpha + 1.0f)
			+ End * (-6.0f * AlphaSquared + 6.0f * Alpha)
			+ EndTangent * (3.0f * AlphaSquared - 2.0f * Alpha);
	}
}

struct ARoadNetworkActor::FGeneratedRunCandidate
{
	TArray<int32> LinkIndices;
	TArray<FGuid> PointIds;
	TSoftClassPtr<AProceduralRoadActor> RoadClass;
	bool bClosedLoop = false;
};

struct ARoadNetworkActor::FStrokeIntersection
{
	int32 StrokeSegmentIndex = INDEX_NONE;
	float StrokeAlpha = 0.0f;
	FGuid ExistingLinkId;
	float ExistingAlpha = 0.0f;
	FVector WorldLocation = FVector::ZeroVector;
	FGuid PointId;
};

ARoadNetworkActor::ARoadNetworkActor()
{
	PrimaryActorTick.bCanEverTick = false;
	SetIsSpatiallyLoaded(false);
	NetworkId = FGuid::NewGuid();
}

bool ARoadNetworkActor::IsEditorOnly() const
{
	return true;
}

const TArray<FRoadNetworkPoint>& ARoadNetworkActor::GetPoints() const
{
	return Points;
}

const TArray<FRoadNetworkLink>& ARoadNetworkActor::GetLinks() const
{
	return Links;
}

const FRoadNetworkPoint* ARoadNetworkActor::FindPoint(const FGuid& PointId) const
{
	return Points.FindByPredicate(
		[PointId](const FRoadNetworkPoint& Point)
		{
			return Point.Id == PointId;
		});
}

const FRoadNetworkLink* ARoadNetworkActor::FindLink(const FGuid& LinkId) const
{
	return Links.FindByPredicate(
		[LinkId](const FRoadNetworkLink& Link)
		{
			return Link.Id == LinkId;
		});
}

void ARoadNetworkActor::SetSelection(const FGuid& PointId, const FGuid& LinkId)
{
	SelectedPointId = PointId;
	SelectedLinkId = LinkId;
}

FGuid ARoadNetworkActor::GetSelectedPointId() const
{
	return SelectedPointId;
}

FGuid ARoadNetworkActor::GetSelectedLinkId() const
{
	return SelectedLinkId;
}

bool ARoadNetworkActor::ProjectToLandscape(
	const FVector& DesiredLocation,
	FVector& OutLocation) const
{
	if (!GetWorld())
	{
		return false;
	}

	TArray<FHitResult> Hits;
	FCollisionQueryParams QueryParams(
		SCENE_QUERY_STAT(RoadPaintingLandscape),
		true,
		this);
	GetWorld()->LineTraceMultiByChannel(
		Hits,
		DesiredLocation + FVector::UpVector * 100000.0f,
		DesiredLocation - FVector::UpVector * 100000.0f,
		ECC_Visibility,
		QueryParams);
	for (const FHitResult& Hit : Hits)
	{
		if (Hit.GetActor() && Hit.GetActor()->IsA<ALandscapeProxy>())
		{
			OutLocation = Hit.ImpactPoint;
			return true;
		}
	}
	return false;
}

void ARoadNetworkActor::SimplifyRange(
	const TArray<FVector>& InputPoints,
	int32 FirstIndex,
	int32 LastIndex,
	TArray<bool>& KeepPoints) const
{
	if (LastIndex <= FirstIndex + 1)
	{
		return;
	}

	float GreatestDistanceSquared = 0.0f;
	int32 GreatestIndex = INDEX_NONE;
	for (int32 PointIndex = FirstIndex + 1; PointIndex < LastIndex; ++PointIndex)
	{
		const FVector Closest = FMath::ClosestPointOnSegment(
			InputPoints[PointIndex],
			InputPoints[FirstIndex],
			InputPoints[LastIndex]);
		const float DistanceSquared = FVector::DistSquared(InputPoints[PointIndex], Closest);
		if (DistanceSquared > GreatestDistanceSquared)
		{
			GreatestDistanceSquared = DistanceSquared;
			GreatestIndex = PointIndex;
		}
	}

	if (GreatestIndex == INDEX_NONE
		|| GreatestDistanceSquared <= FMath::Square(SimplificationTolerance))
	{
		return;
	}

	KeepPoints[GreatestIndex] = true;
	SimplifyRange(InputPoints, FirstIndex, GreatestIndex, KeepPoints);
	SimplifyRange(InputPoints, GreatestIndex, LastIndex, KeepPoints);
}

void ARoadNetworkActor::SimplifyStroke(
	const TArray<FVector>& InputPoints,
	TArray<FVector>& OutPoints) const
{
	OutPoints.Reset();
	if (InputPoints.Num() < 2)
	{
		return;
	}

	TArray<bool> KeepPoints;
	KeepPoints.Init(false, InputPoints.Num());
	KeepPoints[0] = true;
	KeepPoints.Last() = true;
	SimplifyRange(InputPoints, 0, InputPoints.Num() - 1, KeepPoints);
	for (int32 PointIndex = 0; PointIndex < InputPoints.Num(); ++PointIndex)
	{
		if (KeepPoints[PointIndex]
			&& (OutPoints.IsEmpty()
				|| !InputPoints[PointIndex].Equals(OutPoints.Last(), 1.0f)))
		{
			OutPoints.Add(InputPoints[PointIndex]);
		}
	}
}

void ARoadNetworkActor::BuildStrokePreview(
	const TArray<FVector>& SampledPoints,
	TArray<FVector>& OutSimplifiedPoints,
	TArray<FVector>& OutIntersectionPoints) const
{
	SimplifyStroke(SampledPoints, OutSimplifiedPoints);
	OutIntersectionPoints.Reset();
	for (int32 StrokeSegmentIndex = 0;
		StrokeSegmentIndex + 1 < OutSimplifiedPoints.Num();
		++StrokeSegmentIndex)
	{
		for (const FRoadNetworkLink& Link : Links)
		{
			TArray<FVector> LinkSamples;
			SampleLinkPath(Link, LinkSamples);
			for (int32 LinkSampleIndex = 0;
				LinkSampleIndex + 1 < LinkSamples.Num();
				++LinkSampleIndex)
			{
				float StrokeAlpha = 0.0f;
				float LinkAlpha = 0.0f;
				if (!IntersectSegments2D(
					OutSimplifiedPoints[StrokeSegmentIndex],
					OutSimplifiedPoints[StrokeSegmentIndex + 1],
					LinkSamples[LinkSampleIndex],
					LinkSamples[LinkSampleIndex + 1],
					StrokeAlpha,
					LinkAlpha))
				{
					continue;
				}
				const FVector StrokeLocation = FMath::Lerp(
					OutSimplifiedPoints[StrokeSegmentIndex],
					OutSimplifiedPoints[StrokeSegmentIndex + 1],
					StrokeAlpha);
				const FVector LinkLocation = FMath::Lerp(
					LinkSamples[LinkSampleIndex],
					LinkSamples[LinkSampleIndex + 1],
					LinkAlpha);
				if (FMath::Abs(StrokeLocation.Z - LinkLocation.Z)
					<= MaximumJunctionHeightDifference)
				{
					OutIntersectionPoints.AddUnique((StrokeLocation + LinkLocation) * 0.5f);
				}
			}
		}
	}

	for (int32 FirstSegmentIndex = 0;
		FirstSegmentIndex + 1 < OutSimplifiedPoints.Num();
		++FirstSegmentIndex)
	{
		for (int32 SecondSegmentIndex = FirstSegmentIndex + 2;
			SecondSegmentIndex + 1 < OutSimplifiedPoints.Num();
			++SecondSegmentIndex)
		{
			if (FirstSegmentIndex == 0
				&& SecondSegmentIndex + 1 == OutSimplifiedPoints.Num() - 1
				&& OutSimplifiedPoints[0].Equals(OutSimplifiedPoints.Last(), IntersectionMergeRadius))
			{
				continue;
			}
			float FirstAlpha = 0.0f;
			float SecondAlpha = 0.0f;
			if (!IntersectSegments2D(
				OutSimplifiedPoints[FirstSegmentIndex],
				OutSimplifiedPoints[FirstSegmentIndex + 1],
				OutSimplifiedPoints[SecondSegmentIndex],
				OutSimplifiedPoints[SecondSegmentIndex + 1],
				FirstAlpha,
				SecondAlpha))
			{
				continue;
			}
			const FVector FirstLocation = FMath::Lerp(
				OutSimplifiedPoints[FirstSegmentIndex],
				OutSimplifiedPoints[FirstSegmentIndex + 1],
				FirstAlpha);
			const FVector SecondLocation = FMath::Lerp(
				OutSimplifiedPoints[SecondSegmentIndex],
				OutSimplifiedPoints[SecondSegmentIndex + 1],
				SecondAlpha);
			if (FMath::Abs(FirstLocation.Z - SecondLocation.Z)
				<= MaximumJunctionHeightDifference)
			{
				OutIntersectionPoints.AddUnique((FirstLocation + SecondLocation) * 0.5f);
			}
		}
	}
}

bool ARoadNetworkActor::FindSnapPreviewTarget(
	const FVector& WorldLocation,
	FVector& OutWorldLocation) const
{
	float BestDistanceSquared = FMath::Square(SnapRadius);
	bool bFoundTarget = false;
	for (const FRoadNetworkPoint& Point : Points)
	{
		const float DistanceSquared = FVector2D::DistSquared(
			FVector2D(WorldLocation.X, WorldLocation.Y),
			FVector2D(Point.WorldLocation.X, Point.WorldLocation.Y));
		if (DistanceSquared <= BestDistanceSquared
			&& FMath::Abs(WorldLocation.Z - Point.WorldLocation.Z)
				<= MaximumJunctionHeightDifference)
		{
			BestDistanceSquared = DistanceSquared;
			OutWorldLocation = Point.WorldLocation;
			bFoundTarget = true;
		}
	}
	if (bFoundTarget)
	{
		return true;
	}

	for (const FRoadNetworkLink& Link : Links)
	{
		TArray<FVector> LinkSamples;
		SampleLinkPath(Link, LinkSamples);
		for (int32 SampleIndex = 0; SampleIndex + 1 < LinkSamples.Num(); ++SampleIndex)
		{
			float Alpha = 0.0f;
			const float DistanceSquared = DistanceSquaredToSegment2D(
				WorldLocation,
				LinkSamples[SampleIndex],
				LinkSamples[SampleIndex + 1],
				Alpha);
			const FVector Location = FMath::Lerp(
				LinkSamples[SampleIndex],
				LinkSamples[SampleIndex + 1],
				Alpha);
			if (DistanceSquared <= BestDistanceSquared
				&& FMath::Abs(WorldLocation.Z - Location.Z)
					<= MaximumJunctionHeightDifference)
			{
				BestDistanceSquared = DistanceSquared;
				OutWorldLocation = Location;
				bFoundTarget = true;
			}
		}
	}
	return bFoundTarget;
}

FGuid ARoadNetworkActor::AddPoint(const FVector& WorldLocation)
{
	FRoadNetworkPoint& Point = Points.AddDefaulted_GetRef();
	Point.Id = FGuid::NewGuid();
	Point.WorldLocation = WorldLocation;
	return Point.Id;
}

FGuid ARoadNetworkActor::FindOrAddPoint(const FVector& WorldLocation)
{
	const float MergeRadiusSquared = FMath::Square(IntersectionMergeRadius);
	for (const FRoadNetworkPoint& Point : Points)
	{
		if (FVector::DistSquared(Point.WorldLocation, WorldLocation) <= MergeRadiusSquared)
		{
			return Point.Id;
		}
	}
	return AddPoint(WorldLocation);
}

bool ARoadNetworkActor::AddLink(
	const FGuid& StartPointId,
	const FGuid& EndPointId,
	TSubclassOf<AProceduralRoadActor> RoadClass,
	const FRoadNetworkLink* SourceLink)
{
	if (!StartPointId.IsValid() || !EndPointId.IsValid() || StartPointId == EndPointId || !RoadClass)
	{
		return false;
	}
	for (const FRoadNetworkLink& Link : Links)
	{
		if ((Link.StartPointId == StartPointId && Link.EndPointId == EndPointId)
			|| (Link.StartPointId == EndPointId && Link.EndPointId == StartPointId))
		{
			return false;
		}
	}

	FRoadNetworkLink& Link = Links.AddDefaulted_GetRef();
	Link.Id = FGuid::NewGuid();
	Link.StartPointId = StartPointId;
	Link.EndPointId = EndPointId;
	Link.RoadClass = RoadClass;
	if (SourceLink)
	{
		Link.bHasCustomTangents = SourceLink->bHasCustomTangents;
		Link.StartLeaveTangent = SourceLink->StartLeaveTangent;
		Link.EndArriveTangent = SourceLink->EndArriveTangent;
	}
	DirtyLinkIds.Add(Link.Id);
	DirtyPointIds.Add(StartPointId);
	DirtyPointIds.Add(EndPointId);
	return true;
}

void ARoadNetworkActor::SampleLinkPath(
	const FRoadNetworkLink& Link,
	TArray<FVector>& OutWorldPoints) const
{
	OutWorldPoints.Reset();
	for (const FRoadGeneratedRun& Run : GeneratedRuns)
	{
		const int32 SegmentIndex = Run.LinkIds.IndexOfByKey(Link.Id);
		if (SegmentIndex == INDEX_NONE)
		{
			continue;
		}
		if (AProceduralRoadActor* Road = Run.RoadActor.LoadSynchronous())
		{
			if (Road->IsManagedByRoadNetwork(NetworkId))
			{
				Road->SampleRoadSplineSegment(
					SegmentIndex,
					CrossingSampleInterval,
					OutWorldPoints);
			}
		}
		break;
	}

	const FRoadNetworkPoint* StartPoint = FindPoint(Link.StartPointId);
	const FRoadNetworkPoint* EndPoint = FindPoint(Link.EndPointId);
	if (OutWorldPoints.Num() < 2)
	{
		if (StartPoint && EndPoint)
		{
			OutWorldPoints = {StartPoint->WorldLocation, EndPoint->WorldLocation};
		}
		return;
	}
	if (StartPoint
		&& FVector::DistSquared(OutWorldPoints.Last(), StartPoint->WorldLocation)
		< FVector::DistSquared(OutWorldPoints[0], StartPoint->WorldLocation))
	{
		Algo::Reverse(OutWorldPoints);
	}
}

void ARoadNetworkActor::GetLinkWorldSamples(
	const FRoadNetworkLink& Link,
	TArray<FVector>& OutWorldPoints) const
{
	SampleLinkPath(Link, OutWorldPoints);
}

bool ARoadNetworkActor::SplitLinkAtLocation(
	const FGuid& LinkId,
	const FVector& WorldLocation,
	FGuid& OutPointId,
	float SplitAlpha)
{
	const int32 LinkIndex = Links.IndexOfByPredicate(
		[LinkId](const FRoadNetworkLink& Link)
		{
			return Link.Id == LinkId;
		});
	if (LinkIndex == INDEX_NONE)
	{
		return false;
	}

	FRoadNetworkLink SourceLink = Links[LinkIndex];
	const FRoadNetworkPoint* StartPoint = FindPoint(SourceLink.StartPointId);
	const FRoadNetworkPoint* EndPoint = FindPoint(SourceLink.EndPointId);
	if (!StartPoint || !EndPoint)
	{
		return false;
	}
	if (FVector::DistSquared(StartPoint->WorldLocation, WorldLocation)
		<= FMath::Square(IntersectionMergeRadius))
	{
		OutPointId = StartPoint->Id;
		return true;
	}
	if (FVector::DistSquared(EndPoint->WorldLocation, WorldLocation)
		<= FMath::Square(IntersectionMergeRadius))
	{
		OutPointId = EndPoint->Id;
		return true;
	}

	OutPointId = FindOrAddPoint(WorldLocation);
	Links.RemoveAt(LinkIndex);
	UClass* RoadClass = SourceLink.RoadClass.LoadSynchronous();
	SplitAlpha = FMath::Clamp(SplitAlpha, 0.0f, 1.0f);
	if (AddLink(SourceLink.StartPointId, OutPointId, RoadClass, &SourceLink)
		&& SourceLink.bHasCustomTangents)
	{
		FRoadNetworkLink& FirstLink = Links.Last();
		FirstLink.StartLeaveTangent = SourceLink.StartLeaveTangent * SplitAlpha;
		FirstLink.EndArriveTangent = EvaluateHermiteDerivative(
			StartPoint->WorldLocation,
			SourceLink.StartLeaveTangent,
			EndPoint->WorldLocation,
			SourceLink.EndArriveTangent,
			SplitAlpha) * SplitAlpha;
	}
	if (AddLink(OutPointId, SourceLink.EndPointId, RoadClass, &SourceLink)
		&& SourceLink.bHasCustomTangents)
	{
		FRoadNetworkLink& SecondLink = Links.Last();
		const float RemainingAlpha = 1.0f - SplitAlpha;
		SecondLink.StartLeaveTangent = EvaluateHermiteDerivative(
			StartPoint->WorldLocation,
			SourceLink.StartLeaveTangent,
			EndPoint->WorldLocation,
			SourceLink.EndArriveTangent,
			SplitAlpha) * RemainingAlpha;
		SecondLink.EndArriveTangent = SourceLink.EndArriveTangent * RemainingAlpha;
	}
	return true;
}

FGuid ARoadNetworkActor::ResolveStrokeEndpoint(const FVector& WorldLocation)
{
	const float SnapRadiusSquared = FMath::Square(SnapRadius);
	float BestPointDistanceSquared = SnapRadiusSquared;
	FGuid BestPointId;
	for (const FRoadNetworkPoint& Point : Points)
	{
		const float DistanceSquared = FVector2D::DistSquared(
			FVector2D(WorldLocation.X, WorldLocation.Y),
			FVector2D(Point.WorldLocation.X, Point.WorldLocation.Y));
		if (DistanceSquared <= BestPointDistanceSquared
			&& FMath::Abs(WorldLocation.Z - Point.WorldLocation.Z)
				<= MaximumJunctionHeightDifference)
		{
			BestPointDistanceSquared = DistanceSquared;
			BestPointId = Point.Id;
		}
	}
	if (BestPointId.IsValid())
	{
		return BestPointId;
	}

	float BestLinkDistanceSquared = SnapRadiusSquared;
	float BestLinkAlpha = 0.0f;
	FGuid BestLinkId;
	FVector BestLocation;
	for (const FRoadNetworkLink& Link : Links)
	{
		TArray<FVector> LinkSamples;
		SampleLinkPath(Link, LinkSamples);
		for (int32 SampleIndex = 0; SampleIndex + 1 < LinkSamples.Num(); ++SampleIndex)
		{
			float SegmentAlpha = 0.0f;
			const float DistanceSquared = DistanceSquaredToSegment2D(
				WorldLocation,
				LinkSamples[SampleIndex],
				LinkSamples[SampleIndex + 1],
				SegmentAlpha);
			const FVector Location = FMath::Lerp(
				LinkSamples[SampleIndex],
				LinkSamples[SampleIndex + 1],
				SegmentAlpha);
			if (DistanceSquared <= BestLinkDistanceSquared
				&& FMath::Abs(WorldLocation.Z - Location.Z)
					<= MaximumJunctionHeightDifference)
			{
				BestLinkDistanceSquared = DistanceSquared;
				BestLinkAlpha = (SampleIndex + SegmentAlpha) / (LinkSamples.Num() - 1);
				BestLinkId = Link.Id;
				BestLocation = Location;
			}
		}
	}
	if (BestLinkId.IsValid())
	{
		FGuid PointId;
		if (SplitLinkAtLocation(BestLinkId, BestLocation, PointId, BestLinkAlpha))
		{
			return PointId;
		}
	}
	return AddPoint(WorldLocation);
}

bool ARoadNetworkActor::AddPaintedStroke(
	const TArray<FVector>& SampledPoints,
	TSubclassOf<AProceduralRoadActor> RoadClass)
{
	TArray<FVector> SimplifiedPoints;
	SimplifyStroke(SampledPoints, SimplifiedPoints);
	if (SimplifiedPoints.Num() < 2 || !RoadClass)
	{
		return false;
	}

	Modify();
	TArray<FGuid> BasePointIds;
	BasePointIds.SetNum(SimplifiedPoints.Num());
	BasePointIds[0] = ResolveStrokeEndpoint(SimplifiedPoints[0]);
	BasePointIds.Last() = ResolveStrokeEndpoint(SimplifiedPoints.Last());
	SimplifiedPoints[0] = FindPoint(BasePointIds[0])->WorldLocation;
	SimplifiedPoints.Last() = FindPoint(BasePointIds.Last())->WorldLocation;
	for (int32 PointIndex = 1; PointIndex + 1 < SimplifiedPoints.Num(); ++PointIndex)
	{
		BasePointIds[PointIndex] = FindOrAddPoint(SimplifiedPoints[PointIndex]);
	}

	TArray<FStrokeIntersection> Intersections;
	TArray<FRoadNetworkLink> ExistingLinks = Links;
	for (int32 StrokeSegmentIndex = 0;
		StrokeSegmentIndex + 1 < SimplifiedPoints.Num();
		++StrokeSegmentIndex)
	{
		for (const FRoadNetworkLink& ExistingLink : ExistingLinks)
		{
			TArray<FVector> LinkSamples;
			SampleLinkPath(ExistingLink, LinkSamples);
			for (int32 LinkSampleIndex = 0;
				LinkSampleIndex + 1 < LinkSamples.Num();
				++LinkSampleIndex)
			{
				float StrokeAlpha = 0.0f;
				float LinkSampleAlpha = 0.0f;
				if (!IntersectSegments2D(
					SimplifiedPoints[StrokeSegmentIndex],
					SimplifiedPoints[StrokeSegmentIndex + 1],
					LinkSamples[LinkSampleIndex],
					LinkSamples[LinkSampleIndex + 1],
					StrokeAlpha,
					LinkSampleAlpha))
				{
					continue;
				}
				const FVector StrokeLocation = FMath::Lerp(
					SimplifiedPoints[StrokeSegmentIndex],
					SimplifiedPoints[StrokeSegmentIndex + 1],
					StrokeAlpha);
				const FVector ExistingLocation = FMath::Lerp(
					LinkSamples[LinkSampleIndex],
					LinkSamples[LinkSampleIndex + 1],
					LinkSampleAlpha);
				if (FMath::Abs(StrokeLocation.Z - ExistingLocation.Z)
					> MaximumJunctionHeightDifference)
				{
					continue;
				}
				FStrokeIntersection& Intersection = Intersections.AddDefaulted_GetRef();
				Intersection.StrokeSegmentIndex = StrokeSegmentIndex;
				Intersection.StrokeAlpha = StrokeAlpha;
				Intersection.ExistingLinkId = ExistingLink.Id;
				Intersection.ExistingAlpha = (LinkSampleIndex + LinkSampleAlpha)
					/ (LinkSamples.Num() - 1);
				Intersection.WorldLocation = (StrokeLocation + ExistingLocation) * 0.5f;
			}
		}

		for (int32 OtherSegmentIndex = StrokeSegmentIndex + 2;
			OtherSegmentIndex + 1 < SimplifiedPoints.Num();
			++OtherSegmentIndex)
		{
			if (StrokeSegmentIndex == 0
				&& OtherSegmentIndex + 1 == SimplifiedPoints.Num() - 1
				&& SimplifiedPoints[0].Equals(SimplifiedPoints.Last(), IntersectionMergeRadius))
			{
				continue;
			}
			float FirstAlpha = 0.0f;
			float SecondAlpha = 0.0f;
			if (!IntersectSegments2D(
				SimplifiedPoints[StrokeSegmentIndex],
				SimplifiedPoints[StrokeSegmentIndex + 1],
				SimplifiedPoints[OtherSegmentIndex],
				SimplifiedPoints[OtherSegmentIndex + 1],
				FirstAlpha,
				SecondAlpha))
			{
				continue;
			}
			const FVector FirstLocation = FMath::Lerp(
				SimplifiedPoints[StrokeSegmentIndex],
				SimplifiedPoints[StrokeSegmentIndex + 1],
				FirstAlpha);
			const FVector SecondLocation = FMath::Lerp(
				SimplifiedPoints[OtherSegmentIndex],
				SimplifiedPoints[OtherSegmentIndex + 1],
				SecondAlpha);
			if (FMath::Abs(FirstLocation.Z - SecondLocation.Z)
				> MaximumJunctionHeightDifference)
			{
				continue;
			}
			FStrokeIntersection FirstIntersection;
			FirstIntersection.StrokeSegmentIndex = StrokeSegmentIndex;
			FirstIntersection.StrokeAlpha = FirstAlpha;
			FirstIntersection.WorldLocation = (FirstLocation + SecondLocation) * 0.5f;
			Intersections.Add(FirstIntersection);
			FStrokeIntersection SecondIntersection = FirstIntersection;
			SecondIntersection.StrokeSegmentIndex = OtherSegmentIndex;
			SecondIntersection.StrokeAlpha = SecondAlpha;
			Intersections.Add(SecondIntersection);
		}
	}

	for (FStrokeIntersection& Intersection : Intersections)
	{
		FVector LandscapeLocation;
		if (ProjectToLandscape(Intersection.WorldLocation, LandscapeLocation))
		{
			Intersection.WorldLocation = LandscapeLocation;
		}
		Intersection.PointId = FindOrAddPoint(Intersection.WorldLocation);
	}

	TMap<FGuid, TArray<FStrokeIntersection>> LinkIntersections;
	for (const FStrokeIntersection& Intersection : Intersections)
	{
		if (Intersection.ExistingLinkId.IsValid())
		{
			LinkIntersections.FindOrAdd(Intersection.ExistingLinkId).Add(Intersection);
		}
	}
	for (TPair<FGuid, TArray<FStrokeIntersection>>& Pair : LinkIntersections)
	{
		const int32 LinkIndex = Links.IndexOfByPredicate(
			[Pair](const FRoadNetworkLink& Link)
			{
				return Link.Id == Pair.Key;
			});
		if (LinkIndex == INDEX_NONE)
		{
			continue;
		}
		FRoadNetworkLink SourceLink = Links[LinkIndex];
		Links.RemoveAt(LinkIndex);
		Pair.Value.Sort(
			[](const FStrokeIntersection& First, const FStrokeIntersection& Second)
			{
				return First.ExistingAlpha < Second.ExistingAlpha;
			});
		TArray<FGuid> SplitPointIds;
		TArray<float> SplitAlphas;
		SplitPointIds.Add(SourceLink.StartPointId);
		SplitAlphas.Add(0.0f);
		for (const FStrokeIntersection& Intersection : Pair.Value)
		{
			if (SplitPointIds.Last() != Intersection.PointId)
			{
				SplitPointIds.Add(Intersection.PointId);
				SplitAlphas.Add(Intersection.ExistingAlpha);
			}
		}
		if (SplitPointIds.Last() != SourceLink.EndPointId)
		{
			SplitPointIds.Add(SourceLink.EndPointId);
			SplitAlphas.Add(1.0f);
		}
		UClass* ExistingRoadClass = SourceLink.RoadClass.LoadSynchronous();
		const FRoadNetworkPoint* SourceStart = FindPoint(SourceLink.StartPointId);
		const FRoadNetworkPoint* SourceEnd = FindPoint(SourceLink.EndPointId);
		for (int32 SplitIndex = 0; SplitIndex + 1 < SplitPointIds.Num(); ++SplitIndex)
		{
			if (!AddLink(
				SplitPointIds[SplitIndex],
				SplitPointIds[SplitIndex + 1],
				ExistingRoadClass,
				&SourceLink))
			{
				continue;
			}
			if (SourceLink.bHasCustomTangents && SourceStart && SourceEnd)
			{
				const float StartAlpha = SplitAlphas[SplitIndex];
				const float EndAlpha = SplitAlphas[SplitIndex + 1];
				const float AlphaRange = EndAlpha - StartAlpha;
				FRoadNetworkLink& SplitLink = Links.Last();
				SplitLink.StartLeaveTangent = EvaluateHermiteDerivative(
					SourceStart->WorldLocation,
					SourceLink.StartLeaveTangent,
					SourceEnd->WorldLocation,
					SourceLink.EndArriveTangent,
					StartAlpha) * AlphaRange;
				SplitLink.EndArriveTangent = EvaluateHermiteDerivative(
					SourceStart->WorldLocation,
					SourceLink.StartLeaveTangent,
					SourceEnd->WorldLocation,
					SourceLink.EndArriveTangent,
					EndAlpha) * AlphaRange;
			}
		}
	}

	for (int32 StrokeSegmentIndex = 0;
		StrokeSegmentIndex + 1 < BasePointIds.Num();
		++StrokeSegmentIndex)
	{
		TArray<FStrokeIntersection> SegmentIntersections = Intersections.FilterByPredicate(
			[StrokeSegmentIndex](const FStrokeIntersection& Intersection)
			{
				return Intersection.StrokeSegmentIndex == StrokeSegmentIndex;
			});
		SegmentIntersections.Sort(
			[](const FStrokeIntersection& First, const FStrokeIntersection& Second)
			{
				return First.StrokeAlpha < Second.StrokeAlpha;
			});
		TArray<FGuid> SegmentPointIds;
		SegmentPointIds.Add(BasePointIds[StrokeSegmentIndex]);
		for (const FStrokeIntersection& Intersection : SegmentIntersections)
		{
			if (SegmentPointIds.Last() != Intersection.PointId)
			{
				SegmentPointIds.Add(Intersection.PointId);
			}
		}
		if (SegmentPointIds.Last() != BasePointIds[StrokeSegmentIndex + 1])
		{
			SegmentPointIds.Add(BasePointIds[StrokeSegmentIndex + 1]);
		}
		for (int32 PointIndex = 0; PointIndex + 1 < SegmentPointIds.Num(); ++PointIndex)
		{
			AddLink(
				SegmentPointIds[PointIndex],
				SegmentPointIds[PointIndex + 1],
				RoadClass);
		}
	}

	DefaultRoadClass = RoadClass;
	RemoveIsolatedPoints();
	RebuildGeneratedActors();
	return true;
}

void ARoadNetworkActor::BuildAdjacency(
	TMap<FGuid, TArray<int32>>& OutAdjacency) const
{
	OutAdjacency.Reset();
	for (int32 LinkIndex = 0; LinkIndex < Links.Num(); ++LinkIndex)
	{
		OutAdjacency.FindOrAdd(Links[LinkIndex].StartPointId).Add(LinkIndex);
		OutAdjacency.FindOrAdd(Links[LinkIndex].EndPointId).Add(LinkIndex);
	}
}

void ARoadNetworkActor::BuildRunCandidates(
	TArray<FGeneratedRunCandidate>& OutRuns) const
{
	OutRuns.Reset();
	TMap<FGuid, TArray<int32>> Adjacency;
	BuildAdjacency(Adjacency);
	TSet<FGuid> VisitedLinks;

	for (int32 InitialLinkIndex = 0; InitialLinkIndex < Links.Num(); ++InitialLinkIndex)
	{
		if (VisitedLinks.Contains(Links[InitialLinkIndex].Id))
		{
			continue;
		}

		FGeneratedRunCandidate& Run = OutRuns.AddDefaulted_GetRef();
		Run.RoadClass = Links[InitialLinkIndex].RoadClass;
		Run.LinkIndices.Add(InitialLinkIndex);
		Run.PointIds.Add(Links[InitialLinkIndex].StartPointId);
		Run.PointIds.Add(Links[InitialLinkIndex].EndPointId);
		VisitedLinks.Add(Links[InitialLinkIndex].Id);

		auto ExtendRun = [&](bool bAtStart)
		{
			while (true)
			{
				const FGuid CurrentPointId = bAtStart ? Run.PointIds[0] : Run.PointIds.Last();
				const TArray<int32>* ConnectedLinks = Adjacency.Find(CurrentPointId);
				if (!ConnectedLinks || ConnectedLinks->Num() != 2)
				{
					break;
				}

				int32 NextLinkIndex = INDEX_NONE;
				for (int32 ConnectedLinkIndex : *ConnectedLinks)
				{
					if (!Run.LinkIndices.Contains(ConnectedLinkIndex)
						&& Links[ConnectedLinkIndex].RoadClass == Run.RoadClass)
					{
						NextLinkIndex = ConnectedLinkIndex;
						break;
					}
				}
				if (NextLinkIndex == INDEX_NONE)
				{
					break;
				}

				const FGuid NextPointId = GetOtherPoint(Links[NextLinkIndex], CurrentPointId);
				if (NextPointId == (bAtStart ? Run.PointIds.Last() : Run.PointIds[0]))
				{
					Run.LinkIndices.Add(NextLinkIndex);
					VisitedLinks.Add(Links[NextLinkIndex].Id);
					Run.bClosedLoop = true;
					break;
				}

				if (bAtStart)
				{
					Run.LinkIndices.Insert(NextLinkIndex, 0);
					Run.PointIds.Insert(NextPointId, 0);
				}
				else
				{
					Run.LinkIndices.Add(NextLinkIndex);
					Run.PointIds.Add(NextPointId);
				}
				VisitedLinks.Add(Links[NextLinkIndex].Id);
			}
		};

		ExtendRun(false);
		if (!Run.bClosedLoop)
		{
			ExtendRun(true);
		}
	}
}

AProceduralRoadActor* ARoadNetworkActor::FindReusableRoad(
	const FGeneratedRunCandidate& Run,
	TSet<FGuid>& UsedRunIds,
	FGuid& OutRunId) const
{
	UClass* DesiredClass = Run.RoadClass.LoadSynchronous();
	for (const FRoadGeneratedRun& ExistingRun : GeneratedRuns)
	{
		if (UsedRunIds.Contains(ExistingRun.Id))
		{
			continue;
		}
		AProceduralRoadActor* Road = ExistingRun.RoadActor.LoadSynchronous();
		if (!Road
			|| !Road->IsManagedByRoadNetwork(NetworkId)
			|| Road->GetClass() != DesiredClass)
		{
			continue;
		}
		for (int32 LinkIndex : Run.LinkIndices)
		{
			if (ExistingRun.LinkIds.Contains(Links[LinkIndex].Id))
			{
				OutRunId = ExistingRun.Id;
				return Road;
			}
		}
	}
	return nullptr;
}

AProceduralRoadJunctionActor* ARoadNetworkActor::FindReusableJunction(
	const FGuid& NodeId) const
{
	const FRoadGeneratedJunction* GeneratedJunction = GeneratedJunctions.FindByPredicate(
		[NodeId](const FRoadGeneratedJunction& Junction)
		{
			return Junction.NodeId == NodeId;
		});
	AProceduralRoadJunctionActor* Junction = GeneratedJunction
		? GeneratedJunction->JunctionActor.LoadSynchronous()
		: nullptr;
	return Junction && Junction->IsManagedByRoadNetwork(NetworkId)
		? Junction
		: nullptr;
}

void ARoadNetworkActor::RebuildGeneratedActors()
{
	if (!GetWorld() || !NetworkId.IsValid())
	{
		return;
	}

	TArray<AProceduralRoadActor*> ExistingRoadActors;
	for (const FRoadGeneratedRun& ExistingRun : GeneratedRuns)
	{
		if (AProceduralRoadActor* Road = ExistingRun.RoadActor.LoadSynchronous())
		{
			if (Road->IsManagedByRoadNetwork(NetworkId))
			{
				Road->SetEditorRebuildDeferred(true);
				ExistingRoadActors.AddUnique(Road);
			}
		}
	}
	TArray<FGeneratedRunCandidate> RunCandidates;
	BuildRunCandidates(RunCandidates);
	TArray<FRoadGeneratedRun> NewGeneratedRuns;
	TSet<FGuid> UsedRunIds;
	TSet<AProceduralRoadActor*> UsedRoadActors;
	TSet<AProceduralRoadActor*> RoadsToRebuild;
	TMap<FGuid, TArray<TPair<AProceduralRoadActor*, ERoadSplineEndpoint>>> EndpointsByPoint;

	for (const FGeneratedRunCandidate& Run : RunCandidates)
	{
		if (Run.PointIds.Num() < 2)
		{
			continue;
		}
		FGuid RunId;
		AProceduralRoadActor* Road = FindReusableRoad(Run, UsedRunIds, RunId);
		const FRoadGeneratedRun* ExistingRun = GeneratedRuns.FindByPredicate(
			[RunId](const FRoadGeneratedRun& Candidate)
			{
				return Candidate.Id == RunId;
			});
		UClass* RoadClass = Run.RoadClass.LoadSynchronous();
		bool bRunDirty = bForceFullRebuild || !Road;
		if (!Road && RoadClass)
		{
			FActorSpawnParameters SpawnParameters;
			SpawnParameters.ObjectFlags |= RF_Transactional;
			Road = GetWorld()->SpawnActor<AProceduralRoadActor>(
				RoadClass,
				FindPoint(Run.PointIds[0])->WorldLocation,
				FRotator::ZeroRotator,
				SpawnParameters);
			RunId = FGuid::NewGuid();
			if (Road)
			{
				Road->SetActorLabel(FString::Printf(TEXT("PaintedRoad_%s"), *RunId.ToString(EGuidFormats::Short)));
			}
		}
		if (!Road)
		{
			continue;
		}

		Road->SetEditorRebuildDeferred(true);
		Road->SetManagedRoadIdentity(NetworkId, RunId);
		TArray<FGuid> CandidateLinkIds;
		for (int32 LinkIndex : Run.LinkIndices)
		{
			CandidateLinkIds.Add(Links[LinkIndex].Id);
			bRunDirty |= DirtyLinkIds.Contains(Links[LinkIndex].Id);
		}
		bRunDirty |= !ExistingRun || ExistingRun->LinkIds != CandidateLinkIds;
		TArray<FProceduralRoadSplinePoint> SplinePoints;
		for (const FGuid& PointId : Run.PointIds)
		{
			const FRoadNetworkPoint* NetworkPoint = FindPoint(PointId);
			if (!NetworkPoint)
			{
				continue;
			}
			FProceduralRoadSplinePoint& SplinePoint = SplinePoints.AddDefaulted_GetRef();
			SplinePoint.WorldLocation = NetworkPoint->WorldLocation;
			SplinePoint.Rotation = NetworkPoint->Rotation;
			SplinePoint.Scale = NetworkPoint->Scale;
			SplinePoint.Type = NetworkPoint->Type;
		}
		for (int32 PointIndex = 0; PointIndex + 1 < Run.PointIds.Num(); ++PointIndex)
		{
			const FRoadNetworkLink* Link = nullptr;
			for (int32 LinkIndex : Run.LinkIndices)
			{
				if (LinkConnectsPoint(Links[LinkIndex], Run.PointIds[PointIndex])
					&& LinkConnectsPoint(Links[LinkIndex], Run.PointIds[PointIndex + 1]))
				{
					Link = &Links[LinkIndex];
					break;
				}
			}
			if (!Link || !Link->bHasCustomTangents)
			{
				continue;
			}
			const bool bForward = Link->StartPointId == Run.PointIds[PointIndex];
			SplinePoints[PointIndex].WorldLeaveTangent = bForward
				? Link->StartLeaveTangent
				: -Link->EndArriveTangent;
			SplinePoints[PointIndex + 1].WorldArriveTangent = bForward
				? Link->EndArriveTangent
				: -Link->StartLeaveTangent;
			SplinePoints[PointIndex].Type = ESplinePointType::CurveCustomTangent;
			SplinePoints[PointIndex + 1].Type = ESplinePointType::CurveCustomTangent;
		}
		if (bRunDirty)
		{
			Road->SetActorLocation(FindPoint(Run.PointIds[0])->WorldLocation);
			Road->SetRoadSplinePoints(SplinePoints, Run.bClosedLoop, false);
			RoadsToRebuild.Add(Road);
		}

		FRoadGeneratedRun& GeneratedRun = NewGeneratedRuns.AddDefaulted_GetRef();
		GeneratedRun.Id = RunId;
		GeneratedRun.RoadActor = Road;
		GeneratedRun.LinkIds = MoveTemp(CandidateLinkIds);
		UsedRunIds.Add(RunId);
		UsedRoadActors.Add(Road);
		if (!Run.bClosedLoop)
		{
			EndpointsByPoint.FindOrAdd(Run.PointIds[0]).Add(
				TPair<AProceduralRoadActor*, ERoadSplineEndpoint>(Road, ERoadSplineEndpoint::Start));
			EndpointsByPoint.FindOrAdd(Run.PointIds.Last()).Add(
				TPair<AProceduralRoadActor*, ERoadSplineEndpoint>(Road, ERoadSplineEndpoint::End));
		}
	}

	for (AProceduralRoadActor* ExistingRoad : ExistingRoadActors)
	{
		if (ExistingRoad && !UsedRoadActors.Contains(ExistingRoad))
		{
			GetWorld()->EditorDestroyActor(ExistingRoad, true);
		}
	}

	TMap<FGuid, TArray<int32>> Adjacency;
	BuildAdjacency(Adjacency);
	TArray<FRoadGeneratedJunction> NewGeneratedJunctions;
	TSet<AProceduralRoadJunctionActor*> UsedJunctionActors;
	TSet<AProceduralRoadJunctionActor*> JunctionsToRebuild;
	for (const TPair<FGuid, TArray<int32>>& Pair : Adjacency)
	{
		TSet<TSoftClassPtr<AProceduralRoadActor>> ConnectedClasses;
		for (int32 LinkIndex : Pair.Value)
		{
			ConnectedClasses.Add(Links[LinkIndex].RoadClass);
		}
		const bool bNeedsJunction = Pair.Value.Num() >= 3
			|| (Pair.Value.Num() == 2 && ConnectedClasses.Num() > 1);
		if (!bNeedsJunction)
		{
			continue;
		}

		const FRoadNetworkPoint* NetworkPoint = FindPoint(Pair.Key);
		const TArray<TPair<AProceduralRoadActor*, ERoadSplineEndpoint>>* Endpoints = EndpointsByPoint.Find(Pair.Key);
		if (!NetworkPoint || !Endpoints || Endpoints->Num() < 2)
		{
			continue;
		}
		AProceduralRoadJunctionActor* Junction = FindReusableJunction(Pair.Key);
		bool bJunctionDirty = bForceFullRebuild
			|| bForceJunctionRebuild
			|| DirtyPointIds.Contains(Pair.Key)
			|| !Junction;
		UClass* DesiredJunctionClass = JunctionClass.LoadSynchronous();
		if (Junction && DesiredJunctionClass && Junction->GetClass() != DesiredJunctionClass)
		{
			GetWorld()->EditorDestroyActor(Junction, true);
			Junction = nullptr;
			bJunctionDirty = true;
		}
		if (!Junction)
		{
			FActorSpawnParameters SpawnParameters;
			SpawnParameters.ObjectFlags |= RF_Transactional;
			Junction = GetWorld()->SpawnActor<AProceduralRoadJunctionActor>(
				DesiredJunctionClass ? DesiredJunctionClass : AProceduralRoadJunctionActor::StaticClass(),
				NetworkPoint->WorldLocation,
				FRotator::ZeroRotator,
				SpawnParameters);
			if (Junction)
			{
				Junction->SetActorLabel(FString::Printf(TEXT("PaintedJunction_%s"), *Pair.Key.ToString(EGuidFormats::Short)));
			}
		}
		if (!Junction)
		{
			continue;
		}

		TArray<FProceduralRoadJunctionConnection> Connections;
		for (const TPair<AProceduralRoadActor*, ERoadSplineEndpoint>& Endpoint : *Endpoints)
		{
			FProceduralRoadJunctionConnection& Connection = Connections.AddDefaulted_GetRef();
			Connection.Road = Endpoint.Key;
			Connection.Endpoint = Endpoint.Value;
			Connection.TrimDistance = JunctionTrimDistance;
		}
		bJunctionDirty |= !Junction->GetActorLocation().Equals(
			NetworkPoint->WorldLocation,
			1.0f);
		const TArray<FProceduralRoadJunctionConnection>& ExistingConnections =
			Junction->GetRoadConnections();
		if (ExistingConnections.Num() != Connections.Num())
		{
			bJunctionDirty = true;
		}
		else
		{
			for (const FProceduralRoadJunctionConnection& Connection : Connections)
			{
				const bool bHasMatchingConnection = ExistingConnections.ContainsByPredicate(
					[Connection](const FProceduralRoadJunctionConnection& ExistingConnection)
					{
						return ExistingConnection.Road == Connection.Road
							&& ExistingConnection.Endpoint == Connection.Endpoint
							&& FMath::IsNearlyEqual(
								ExistingConnection.TrimDistance,
								Connection.TrimDistance);
					});
				if (!bHasMatchingConnection)
				{
					bJunctionDirty = true;
					break;
				}
			}
		}
		if (bJunctionDirty)
		{
			Junction->SetActorLocation(NetworkPoint->WorldLocation);
			Junction->SetManagedConnections(Connections, NetworkId, Pair.Key, false);
			JunctionsToRebuild.Add(Junction);
		}
		FRoadGeneratedJunction& GeneratedJunction = NewGeneratedJunctions.AddDefaulted_GetRef();
		GeneratedJunction.NodeId = Pair.Key;
		GeneratedJunction.JunctionActor = Junction;
		UsedJunctionActors.Add(Junction);
	}

	TArray<TPair<FVector, float>> DirtyJunctionNeighborhoods;
	for (AProceduralRoadJunctionActor* Junction : JunctionsToRebuild)
	{
		DirtyJunctionNeighborhoods.Add(TPair<FVector, float>(
			Junction->GetActorLocation(),
			Junction->GetNearbyJunctionSearchRadius()));
	}
	for (const FRoadGeneratedJunction& ExistingJunction : GeneratedJunctions)
	{
		AProceduralRoadJunctionActor* Junction = ExistingJunction.JunctionActor.LoadSynchronous();
		if (Junction
			&& Junction->IsManagedByRoadNetwork(NetworkId)
			&& !UsedJunctionActors.Contains(Junction))
		{
			DirtyJunctionNeighborhoods.Add(TPair<FVector, float>(
				Junction->GetActorLocation(),
				Junction->GetNearbyJunctionSearchRadius()));
			GetWorld()->EditorDestroyActor(Junction, true);
		}
	}
	for (const FRoadGeneratedJunction& GeneratedJunction : NewGeneratedJunctions)
	{
		AProceduralRoadJunctionActor* Junction =
			GeneratedJunction.JunctionActor.LoadSynchronous();
		if (!Junction || JunctionsToRebuild.Contains(Junction))
		{
			continue;
		}
		for (const TPair<FVector, float>& DirtyNeighborhood : DirtyJunctionNeighborhoods)
		{
			const float RebuildRadius = FMath::Max(
				Junction->GetNearbyJunctionSearchRadius(),
				DirtyNeighborhood.Value);
			if (FVector2D::DistSquared(
				FVector2D(Junction->GetActorLocation()),
				FVector2D(DirtyNeighborhood.Key))
				<= FMath::Square(RebuildRadius))
			{
				JunctionsToRebuild.Add(Junction);
				break;
			}
		}
	}
	for (AProceduralRoadJunctionActor* Junction : JunctionsToRebuild)
	{
		Junction->SynchronizeJunctionRoadTrims();
		for (const FProceduralRoadJunctionConnection& Connection :
			Junction->GetRoadConnections())
		{
			RoadsToRebuild.Add(Connection.Road);
		}
	}
	for (AProceduralRoadActor* Road : UsedRoadActors)
	{
		if (RoadsToRebuild.Contains(Road))
		{
			Road->RebuildRoad();
		}
		Road->SetEditorRebuildDeferred(false);
	}
	for (AProceduralRoadJunctionActor* Junction : JunctionsToRebuild)
	{
		Junction->RebuildJunctionMesh();
	}
	GeneratedRuns = MoveTemp(NewGeneratedRuns);
	GeneratedJunctions = MoveTemp(NewGeneratedJunctions);
	DirtyLinkIds.Reset();
	DirtyPointIds.Reset();
	bForceFullRebuild = false;
	bForceJunctionRebuild = false;
	MarkPackageDirty();
}

void ARoadNetworkActor::RemoveIsolatedPoints()
{
	Points.RemoveAll(
		[this](const FRoadNetworkPoint& Point)
		{
			return !Links.ContainsByPredicate(
				[Point](const FRoadNetworkLink& Link)
				{
					return LinkConnectsPoint(Link, Point.Id);
				});
		});
}

void ARoadNetworkActor::MarkPointConnectionsDirty(const FGuid& PointId)
{
	DirtyPointIds.Add(PointId);
	for (const FRoadNetworkLink& Link : Links)
	{
		if (LinkConnectsPoint(Link, PointId))
		{
			DirtyLinkIds.Add(Link.Id);
		}
	}
}

bool ARoadNetworkActor::MovePointToLandscape(
	const FGuid& PointId,
	const FVector& WorldLocation)
{
	FRoadNetworkPoint* Point = Points.FindByPredicate(
		[PointId](const FRoadNetworkPoint& Candidate)
		{
			return Candidate.Id == PointId;
		});
	FVector LandscapeLocation;
	if (!Point || !ProjectToLandscape(WorldLocation, LandscapeLocation))
	{
		return false;
	}
	Modify();
	Point->WorldLocation = LandscapeLocation;
	MarkPointConnectionsDirty(PointId);
	return true;
}

bool ARoadNetworkActor::DeletePoint(const FGuid& PointId)
{
	TArray<int32> ConnectedLinkIndices;
	for (int32 LinkIndex = 0; LinkIndex < Links.Num(); ++LinkIndex)
	{
		if (LinkConnectsPoint(Links[LinkIndex], PointId))
		{
			ConnectedLinkIndices.Add(LinkIndex);
		}
	}
	if (ConnectedLinkIndices.IsEmpty())
	{
		return false;
	}
	Modify();
	bForceFullRebuild = true;
	if (ConnectedLinkIndices.Num() == 2
		&& Links[ConnectedLinkIndices[0]].RoadClass == Links[ConnectedLinkIndices[1]].RoadClass)
	{
		FRoadNetworkLink FirstLink = Links[ConnectedLinkIndices[0]];
		FRoadNetworkLink SecondLink = Links[ConnectedLinkIndices[1]];
		const FGuid FirstOtherPoint = GetOtherPoint(FirstLink, PointId);
		const FGuid SecondOtherPoint = GetOtherPoint(SecondLink, PointId);
		ConnectedLinkIndices.Sort(TGreater<int32>());
		for (int32 LinkIndex : ConnectedLinkIndices)
		{
			Links.RemoveAt(LinkIndex);
		}
		AddLink(FirstOtherPoint, SecondOtherPoint, FirstLink.RoadClass.LoadSynchronous());
	}
	else
	{
		ConnectedLinkIndices.Sort(TGreater<int32>());
		for (int32 LinkIndex : ConnectedLinkIndices)
		{
			Links.RemoveAt(LinkIndex);
		}
	}
	Points.RemoveAll(
		[PointId](const FRoadNetworkPoint& Point)
		{
			return Point.Id == PointId;
		});
	RemoveIsolatedPoints();
	SetSelection(FGuid(), FGuid());
	RebuildGeneratedActors();
	return true;
}

bool ARoadNetworkActor::DeleteLink(const FGuid& LinkId)
{
	Modify();
	bForceFullRebuild = true;
	const int32 RemovedCount = Links.RemoveAll(
		[LinkId](const FRoadNetworkLink& Link)
		{
			return Link.Id == LinkId;
		});
	if (RemovedCount == 0)
	{
		return false;
	}
	RemoveIsolatedPoints();
	SetSelection(FGuid(), FGuid());
	RebuildGeneratedActors();
	return true;
}

bool ARoadNetworkActor::DeleteSelection()
{
	if (SelectedPointId.IsValid())
	{
		return DeletePoint(SelectedPointId);
	}
	if (SelectedLinkId.IsValid())
	{
		return DeleteLink(SelectedLinkId);
	}
	return false;
}

bool ARoadNetworkActor::AdoptSelectedRoads()
{
	if (!GEditor)
	{
		return false;
	}
	TArray<AProceduralRoadActor*> SelectedRoads;
	TArray<AProceduralRoadJunctionActor*> SelectedJunctions;
	for (FSelectionIterator Iterator(*GEditor->GetSelectedActors()); Iterator; ++Iterator)
	{
		if (AProceduralRoadActor* Road = Cast<AProceduralRoadActor>(*Iterator))
		{
			SelectedRoads.Add(Road);
		}
		else if (AProceduralRoadJunctionActor* Junction = Cast<AProceduralRoadJunctionActor>(*Iterator))
		{
			SelectedJunctions.Add(Junction);
		}
	}
	if (SelectedRoads.IsEmpty())
	{
		return false;
	}
	for (AProceduralRoadJunctionActor* Junction : SelectedJunctions)
	{
		for (const FProceduralRoadJunctionConnection& Connection : Junction->GetRoadConnections())
		{
			if (Connection.Road && !SelectedRoads.Contains(Connection.Road))
			{
				UE_LOG(LogRoadPainting, Error,
					TEXT("Cannot partially adopt junction %s; also select road %s."),
					*Junction->GetActorLabel(),
					*Connection.Road->GetActorLabel());
				return false;
			}
		}
	}

	Modify();
	bForceFullRebuild = true;
	for (AProceduralRoadActor* Road : SelectedRoads)
	{
		if (!Road || Road->IsManagedByRoadNetwork(NetworkId))
		{
			continue;
		}
		TArray<FProceduralRoadSplinePoint> SplinePoints;
		Road->GetRoadSplinePoints(SplinePoints);
		if (SplinePoints.Num() < 2)
		{
			continue;
		}
		TArray<FGuid> PointIds;
		for (const FProceduralRoadSplinePoint& SplinePoint : SplinePoints)
		{
			const FGuid PointId = FindOrAddPoint(SplinePoint.WorldLocation);
			FRoadNetworkPoint* NetworkPoint = Points.FindByPredicate(
				[PointId](const FRoadNetworkPoint& Point)
				{
					return Point.Id == PointId;
				});
			NetworkPoint->Rotation = SplinePoint.Rotation;
			NetworkPoint->Scale = SplinePoint.Scale;
			NetworkPoint->Type = SplinePoint.Type;
			PointIds.Add(PointId);
		}

		FRoadGeneratedRun& GeneratedRun = GeneratedRuns.AddDefaulted_GetRef();
		GeneratedRun.Id = FGuid::NewGuid();
		GeneratedRun.RoadActor = Road;
		for (int32 PointIndex = 0; PointIndex + 1 < PointIds.Num(); ++PointIndex)
		{
			FRoadNetworkLink Link;
			Link.Id = FGuid::NewGuid();
			Link.StartPointId = PointIds[PointIndex];
			Link.EndPointId = PointIds[PointIndex + 1];
			Link.RoadClass = Road->GetClass();
			Link.bHasCustomTangents =
				SplinePoints[PointIndex].Type == ESplinePointType::CurveCustomTangent
				|| SplinePoints[PointIndex + 1].Type == ESplinePointType::CurveCustomTangent;
			Link.StartLeaveTangent = SplinePoints[PointIndex].WorldLeaveTangent;
			Link.EndArriveTangent = SplinePoints[PointIndex + 1].WorldArriveTangent;
			Links.Add(Link);
			GeneratedRun.LinkIds.Add(Link.Id);
		}
		if (Road->IsRoadSplineClosedLoop())
		{
			FRoadNetworkLink Link;
			Link.Id = FGuid::NewGuid();
			Link.StartPointId = PointIds.Last();
			Link.EndPointId = PointIds[0];
			Link.RoadClass = Road->GetClass();
			Link.bHasCustomTangents =
				SplinePoints.Last().Type == ESplinePointType::CurveCustomTangent
				|| SplinePoints[0].Type == ESplinePointType::CurveCustomTangent;
			Link.StartLeaveTangent = SplinePoints.Last().WorldLeaveTangent;
			Link.EndArriveTangent = SplinePoints[0].WorldArriveTangent;
			Links.Add(Link);
			GeneratedRun.LinkIds.Add(Link.Id);
		}
		Road->SetManagedRoadIdentity(NetworkId, GeneratedRun.Id);
		DefaultRoadClass = Road->GetClass();
	}
	for (AProceduralRoadJunctionActor* Junction : SelectedJunctions)
	{
		float BestDistanceSquared = TNumericLimits<float>::Max();
		FGuid BestPointId;
		for (const FRoadNetworkPoint& Point : Points)
		{
			const float DistanceSquared = FVector::DistSquared(
				Point.WorldLocation,
				Junction->GetActorLocation());
			if (DistanceSquared < BestDistanceSquared)
			{
				BestDistanceSquared = DistanceSquared;
				BestPointId = Point.Id;
			}
		}
		if (BestPointId.IsValid())
		{
			Junction->SetManagedConnections(
				Junction->GetRoadConnections(),
				NetworkId,
				BestPointId,
				false);
			FRoadGeneratedJunction& GeneratedJunction = GeneratedJunctions.AddDefaulted_GetRef();
			GeneratedJunction.NodeId = BestPointId;
			GeneratedJunction.JunctionActor = Junction;
		}
	}
	RebuildGeneratedActors();
	return true;
}

bool ARoadNetworkActor::ValidateGraph(FString& OutErrors) const
{
	if (!NetworkId.IsValid())
	{
		OutErrors += TEXT("Network has no valid GUID.\n");
	}
	TSet<FGuid> PointIds;
	TSet<FGuid> LinkIds;
	for (const FRoadNetworkPoint& Point : Points)
	{
		if (!Point.Id.IsValid() || PointIds.Contains(Point.Id))
		{
			OutErrors += TEXT("Invalid or duplicate point GUID.\n");
		}
		PointIds.Add(Point.Id);
	}
	for (const FRoadNetworkLink& Link : Links)
	{
		if (!Link.Id.IsValid() || LinkIds.Contains(Link.Id))
		{
			OutErrors += TEXT("Invalid or duplicate link GUID.\n");
		}
		if (!PointIds.Contains(Link.StartPointId) || !PointIds.Contains(Link.EndPointId))
		{
			OutErrors += TEXT("Link references a missing point.\n");
		}
		if (!Link.RoadClass.IsValid() && Link.RoadClass.IsNull())
		{
			OutErrors += TEXT("Link has no road class.\n");
		}
		LinkIds.Add(Link.Id);
	}
	TSet<FGuid> RunIds;
	for (const FRoadGeneratedRun& Run : GeneratedRuns)
	{
		if (!Run.Id.IsValid() || RunIds.Contains(Run.Id))
		{
			OutErrors += TEXT("Invalid or duplicate generated run GUID.\n");
		}
		for (const FGuid& LinkId : Run.LinkIds)
		{
			if (!LinkIds.Contains(LinkId))
			{
				OutErrors += TEXT("Generated run references a missing link.\n");
			}
		}
		AProceduralRoadActor* Road = Run.RoadActor.LoadSynchronous();
		if (!Road)
		{
			OutErrors += TEXT("Generated run has a missing road actor.\n");
		}
		else if (!Road->IsManagedByRoadNetwork(NetworkId)
			|| Road->GetManagedRoadRunId() != Run.Id)
		{
			OutErrors += TEXT("Generated road actor identity does not match its network run.\n");
		}
		RunIds.Add(Run.Id);
	}
	TSet<FGuid> JunctionNodeIds;
	for (const FRoadGeneratedJunction& GeneratedJunction : GeneratedJunctions)
	{
		if (!PointIds.Contains(GeneratedJunction.NodeId)
			|| JunctionNodeIds.Contains(GeneratedJunction.NodeId))
		{
			OutErrors += TEXT("Generated junction has an invalid or duplicate node GUID.\n");
		}
		AProceduralRoadJunctionActor* Junction =
			GeneratedJunction.JunctionActor.LoadSynchronous();
		if (!Junction)
		{
			OutErrors += TEXT("Generated junction actor is missing.\n");
		}
		else if (!Junction->IsManagedByRoadNetwork(NetworkId)
			|| Junction->GetManagedRoadNodeId() != GeneratedJunction.NodeId)
		{
			OutErrors += TEXT("Generated junction actor identity does not match its graph node.\n");
		}
		JunctionNodeIds.Add(GeneratedJunction.NodeId);
	}
	return OutErrors.IsEmpty();
}

void ARoadNetworkActor::RebuildDirty()
{
	bForceJunctionRebuild = true;
	RebuildGeneratedActors();
}

void ARoadNetworkActor::RebuildAll()
{
	bForceFullRebuild = true;
	RebuildGeneratedActors();
}

void ARoadNetworkActor::AdoptSelected()
{
	const FScopedTransaction Transaction(NSLOCTEXT("RoadPainting", "AdoptRoads", "Adopt Roads"));
	AdoptSelectedRoads();
}

void ARoadNetworkActor::ValidateNetwork()
{
	FString Errors;
	if (ValidateGraph(Errors))
	{
		UE_LOG(LogRoadPainting, Display, TEXT("Road network %s is valid."), *GetActorLabel());
	}
	else
	{
		UE_LOG(LogRoadPainting, Error, TEXT("Road network %s is invalid:\n%s"), *GetActorLabel(), *Errors);
	}
}
