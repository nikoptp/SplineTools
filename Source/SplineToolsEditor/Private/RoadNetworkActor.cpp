#include "RoadNetworkActor.h"

#include "RoadPaintingLandscapeQuery.h"
#include "RoadNetworkLandscapeBrush.h"

#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Landscape.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeProxy.h"
#include "ProceduralRoadJunctionActor.h"
#include "RoadPaintingIterationBudget.h"
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

struct ARoadNetworkActor::FLandscapePaintRebuildState
{
	struct FBrushEditLayerGroup
	{
		FName EditLayerName;
		TArray<FRoadLandscapeBrushLayer> PaintLayers;
	};

	enum class EPhase : uint8
	{
		BuildPaintLayers,
		SynchronizeLandscapes,
		CleanupBrushes,
		Complete
	};

	EPhase Phase = EPhase::BuildPaintLayers;
	TArray<FBrushEditLayerGroup> BrushGroups;
	TArray<FString> BrushMessages;
	TSet<UClass*> NarrowPaintProfileWarnings;
	int32 PaintedLinkCount = 0;
	int32 LinkIndex = 0;
	int32 LinkSampleIndex = 0;
	int32 CurrentPaintGroupIndex = INDEX_NONE;
	int32 CurrentPaintLayerIndex = INDEX_NONE;
	float CurrentCoreHalfWidth = 0.0f;
	float CurrentFalloff = 0.0f;
	bool bCurrentLinkPrepared = false;
	bool bCurrentLinkPainted = false;
	TArray<FVector> CurrentLinkSamples;

	TArray<ALandscape*> Landscapes;
	int32 LandscapeIndex = 0;
	int32 BrushGroupIndex = 0;
	int32 PaintLayerIndex = 0;
	int32 ExistingBrushIndex = 0;
	int32 CleanupBrushIndex = 0;
	TArray<FName> MaterialLayers;
	ULandscapeInfo* LandscapeInfo = nullptr;
	TArray<FRoadLandscapeBrushLayer> ValidPaintLayers;
	int32 EditLayerIndex = INDEX_NONE;
	bool bLandscapePrepared = false;
	bool bLandscapeSkipped = false;
	bool bGroupPrepared = false;
	bool bEditLayerResolved = false;
	bool bGroupFailed = false;
	bool bBrushApplied = false;
	ARoadNetworkLandscapeBrush* CurrentBrush = nullptr;

	TArray<ARoadNetworkLandscapeBrush*> ExistingBrushes;
	TSet<ARoadNetworkLandscapeBrush*> UsedBrushes;
	TArray<FRoadGeneratedLandscapeBrush> NewGeneratedBrushes;
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

void ARoadNetworkActor::Destroyed()
{
#if WITH_EDITOR
	CancelQueuedUndoRebuild();
	CancelLandscapeMaterialPaintRebuild();
	CancelLandscapePaintUpdate();
	TArray<ARoadNetworkLandscapeBrush*> ManagedBrushes;
	FindLoadedManagedLandscapeBrushes(ManagedBrushes);
	for (ARoadNetworkLandscapeBrush* Brush : ManagedBrushes)
	{
		if (ALandscape* Landscape = Brush->GetOwningLandscape())
		{
			Landscape->Modify();
			Landscape->RemoveBrush(Brush);
		}
		Brush->Destroy();
	}
#endif
	Super::Destroyed();
}

#if WITH_EDITOR
void ARoadNetworkActor::PostEditUndo()
{
	Super::PostEditUndo();
	QueueUndoRebuild();
}

void ARoadNetworkActor::QueueUndoRebuild()
{
	CancelQueuedUndoRebuild();
	UndoRebuildTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateWeakLambda(
			this,
			[this](float)
			{
				UndoRebuildTickerHandle.Reset();
				if (!IsTemplate() && GetWorld())
				{
					SetSelection(FGuid(), FGuid());
					bForceFullRebuild = true;
					RebuildGeneratedActors();
				}
				return false;
			}),
		0.0f);
}

void ARoadNetworkActor::CancelQueuedUndoRebuild()
{
	if (!UndoRebuildTickerHandle.IsValid())
	{
		return;
	}

	FTSTicker::GetCoreTicker().RemoveTicker(UndoRebuildTickerHandle);
	UndoRebuildTickerHandle.Reset();
}
#endif

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
	TArray<FGuid> PointIds;
	TArray<FGuid> LinkIds;
	if (PointId.IsValid())
	{
		PointIds.Add(PointId);
	}
	if (LinkId.IsValid())
	{
		LinkIds.Add(LinkId);
	}
	SetSelection(PointIds, LinkIds);
}

void ARoadNetworkActor::SetSelection(
	const TArray<FGuid>& PointIds,
	const TArray<FGuid>& LinkIds)
{
	SelectedPointIds.Reset();
	for (const FGuid& PointId : PointIds)
	{
		if (PointId.IsValid() && FindPoint(PointId))
		{
			SelectedPointIds.AddUnique(PointId);
		}
	}

	SelectedLinkIds.Reset();
	for (const FGuid& LinkId : LinkIds)
	{
		if (LinkId.IsValid() && FindLink(LinkId))
		{
			SelectedLinkIds.AddUnique(LinkId);
		}
	}
	RefreshSelectionAliases();
}

void ARoadNetworkActor::RefreshSelectionAliases()
{
	SelectedPointId = SelectedPointIds.IsEmpty()
		? FGuid()
		: SelectedPointIds[0];
	SelectedLinkId = SelectedLinkIds.IsEmpty()
		? FGuid()
		: SelectedLinkIds[0];
}

bool ARoadNetworkActor::IsPointSelected(const FGuid& PointId) const
{
	return SelectedPointIds.Contains(PointId);
}

bool ARoadNetworkActor::IsLinkSelected(const FGuid& LinkId) const
{
	return SelectedLinkIds.Contains(LinkId);
}

const TArray<FGuid>& ARoadNetworkActor::GetSelectedPointIds() const
{
	return SelectedPointIds;
}

const TArray<FGuid>& ARoadNetworkActor::GetSelectedLinkIds() const
{
	return SelectedLinkIds;
}

bool ARoadNetworkActor::MoveSelectedElementsBy(const FVector& WorldDelta)
{
	TArray<FGuid> PointIds = SelectedPointIds;
	for (const FGuid& LinkId : SelectedLinkIds)
	{
		const FRoadNetworkLink* Link = FindLink(LinkId);
		if (Link)
		{
			PointIds.AddUnique(Link->StartPointId);
			PointIds.AddUnique(Link->EndPointId);
		}
	}

	bool bMoved = false;
	for (const FGuid& PointId : PointIds)
	{
		FRoadNetworkPoint* Point = Points.FindByPredicate(
			[PointId](const FRoadNetworkPoint& Candidate)
			{
				return Candidate.Id == PointId;
			});
		if (!Point)
		{
			continue;
		}

		FVector LandscapeLocation;
		if (!ProjectToLandscape(Point->WorldLocation + WorldDelta, LandscapeLocation))
		{
			continue;
		}
		if (Point->WorldLocation.Equals(LandscapeLocation))
		{
			continue;
		}
		if (!bMoved)
		{
			Modify();
			bMoved = true;
		}
		Point->WorldLocation = LandscapeLocation;
		MarkPointConnectionsDirty(PointId);
	}
	return bMoved;
}

FGuid ARoadNetworkActor::GetSelectedPointId() const
{
	return SelectedPointId;
}

FGuid ARoadNetworkActor::GetSelectedLinkId() const
{
	return SelectedLinkId;
}

int32 ARoadNetworkActor::GetGeneratedJunctionCount() const
{
	return GeneratedJunctions.Num();
}

bool ARoadNetworkActor::HasPendingRebuild() const
{
	return bForceFullRebuild
		|| bForceJunctionRebuild
		|| bLandscapePaintRebuildPending
		|| !DirtyLinkIds.IsEmpty()
		|| !DirtyPointIds.IsEmpty();
}

bool ARoadNetworkActor::ValidateNetworkGraph(FString& OutErrors) const
{
	return ValidateGraph(OutErrors);
}

FText ARoadNetworkActor::GetLandscapePaintStatusText() const
{
	return LandscapePaintStatus.IsEmpty()
		? NSLOCTEXT("RoadPainting", "LandscapePaintNotRun", "Landscape paint has not been rebuilt.")
		: FText::FromString(LandscapePaintStatus);
}

bool ARoadNetworkActor::InsertPointOnLink(const FGuid& LinkId)
{
	const FRoadNetworkLink* Link = FindLink(LinkId);
	if (!Link)
	{
		return false;
	}

	TArray<FVector> LinkSamples;
	GetLinkWorldSamples(*Link, LinkSamples);
	if (LinkSamples.Num() < 2)
	{
		return false;
	}

	const FVector Midpoint = LinkSamples[LinkSamples.Num() / 2];
	float BestDistanceSquared = MAX_FLT;
	float BestAlpha = 0.5f;
	FVector BestLocation = Midpoint;
	for (int32 SampleIndex = 0; SampleIndex + 1 < LinkSamples.Num(); ++SampleIndex)
	{
		float SegmentAlpha = 0.0f;
		const float DistanceSquared = DistanceSquaredToSegment2D(
			Midpoint,
			LinkSamples[SampleIndex],
			LinkSamples[SampleIndex + 1],
			SegmentAlpha);
		if (DistanceSquared < BestDistanceSquared)
		{
			BestDistanceSquared = DistanceSquared;
			BestLocation = FMath::Lerp(
				LinkSamples[SampleIndex],
				LinkSamples[SampleIndex + 1],
				SegmentAlpha);
			BestAlpha = (SampleIndex + SegmentAlpha)
				/ static_cast<float>(LinkSamples.Num() - 1);
		}
	}

	Modify();
	FGuid PointId;
	if (!SplitLinkAtLocation(LinkId, BestLocation, PointId, BestAlpha))
	{
		return false;
	}
	SetSelection(PointId, FGuid());
	bForceFullRebuild = true;
	RebuildGeneratedActors();
	return true;
}

bool ARoadNetworkActor::ProjectToLandscape(
	const FVector& DesiredLocation,
	FVector& OutLocation) const
{
	if (!GetWorld())
	{
		return false;
	}

	float HitDistance = 0.0f;
	return RoadPaintingLandscapeQuery::FindSurfaceAlongSegment(
		GetWorld(),
		DesiredLocation + FVector::UpVector * 100000.0f,
		DesiredLocation - FVector::UpVector * 100000.0f,
		OutLocation,
		HitDistance,
		this);
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

bool ARoadNetworkActor::BuildStrokePreview(
	const TArray<FVector>& SampledPoints,
	FRoadStrokePreviewState& PreviewState,
	TArray<FVector>& OutSimplifiedPoints,
	TArray<FVector>& OutIntersectionPoints) const
{
	if (!PreviewState.bInitialized || PreviewState.InputPoints != SampledPoints)
	{
		PreviewState.Reset();
		PreviewState.InputPoints = SampledPoints;
		SimplifyStroke(SampledPoints, PreviewState.SimplifiedPoints);
		PreviewState.bInitialized = true;
	}

	int32 Iterations = 0;
	while (Iterations < RoadPainting::MaxIterationsPerFrame
		&& !PreviewState.bComplete)
	{
		++Iterations;
		if (!PreviewState.bTestingSelfIntersections)
		{
			if (PreviewState.StrokeSegmentIndex + 1
				>= PreviewState.SimplifiedPoints.Num())
			{
				PreviewState.bTestingSelfIntersections = true;
				PreviewState.FirstSegmentIndex = 0;
				PreviewState.SecondSegmentIndex = 2;
				continue;
			}
			if (PreviewState.LinkIndex >= Links.Num())
			{
				++PreviewState.StrokeSegmentIndex;
				PreviewState.LinkIndex = 0;
				PreviewState.CurrentLinkIndex = INDEX_NONE;
				continue;
			}
			if (PreviewState.CurrentLinkIndex != PreviewState.LinkIndex)
			{
				PreviewState.CurrentLinkIndex = PreviewState.LinkIndex;
				PreviewState.LinkSampleIndex = 0;
				if (PreviewState.CachedLinkSamples.Num() != Links.Num())
				{
					PreviewState.CachedLinkSamples.SetNum(Links.Num());
					PreviewState.CachedLinkSampleFlags.Init(false, Links.Num());
				}
				if (!PreviewState.CachedLinkSampleFlags[PreviewState.LinkIndex])
				{
					SampleLinkPath(
						Links[PreviewState.LinkIndex],
						PreviewState.CachedLinkSamples[PreviewState.LinkIndex]);
					PreviewState.CachedLinkSampleFlags[PreviewState.LinkIndex] = true;
				}
			}
			if (PreviewState.LinkSampleIndex + 1
				>= PreviewState.CachedLinkSamples[PreviewState.CurrentLinkIndex].Num())
			{
				++PreviewState.LinkIndex;
				PreviewState.CurrentLinkIndex = INDEX_NONE;
				continue;
			}

			float StrokeAlpha = 0.0f;
			float LinkAlpha = 0.0f;
			if (IntersectSegments2D(
				PreviewState.SimplifiedPoints[PreviewState.StrokeSegmentIndex],
				PreviewState.SimplifiedPoints[PreviewState.StrokeSegmentIndex + 1],
				PreviewState.CachedLinkSamples[PreviewState.CurrentLinkIndex][PreviewState.LinkSampleIndex],
				PreviewState.CachedLinkSamples[PreviewState.CurrentLinkIndex][PreviewState.LinkSampleIndex + 1],
				StrokeAlpha,
				LinkAlpha))
			{
				const FVector StrokeLocation = FMath::Lerp(
					PreviewState.SimplifiedPoints[PreviewState.StrokeSegmentIndex],
					PreviewState.SimplifiedPoints[PreviewState.StrokeSegmentIndex + 1],
					StrokeAlpha);
				const FVector LinkLocation = FMath::Lerp(
					PreviewState.CachedLinkSamples[PreviewState.CurrentLinkIndex][PreviewState.LinkSampleIndex],
					PreviewState.CachedLinkSamples[PreviewState.CurrentLinkIndex][PreviewState.LinkSampleIndex + 1],
					LinkAlpha);
				if (FMath::Abs(StrokeLocation.Z - LinkLocation.Z)
					<= MaximumJunctionHeightDifference)
				{
					PreviewState.IntersectionPoints.AddUnique(
						(StrokeLocation + LinkLocation) * 0.5f);
				}
			}
			++PreviewState.LinkSampleIndex;
			continue;
		}

		if (PreviewState.FirstSegmentIndex + 1
			>= PreviewState.SimplifiedPoints.Num())
		{
			PreviewState.bComplete = true;
			continue;
		}
		if (PreviewState.SecondSegmentIndex + 1
			>= PreviewState.SimplifiedPoints.Num())
		{
			++PreviewState.FirstSegmentIndex;
			PreviewState.SecondSegmentIndex = PreviewState.FirstSegmentIndex + 2;
			continue;
		}
		if (PreviewState.FirstSegmentIndex == 0
			&& PreviewState.SecondSegmentIndex + 1
			== PreviewState.SimplifiedPoints.Num() - 1
			&& PreviewState.SimplifiedPoints[0].Equals(
				PreviewState.SimplifiedPoints.Last(),
				IntersectionMergeRadius))
		{
			++PreviewState.SecondSegmentIndex;
			continue;
		}

		float FirstAlpha = 0.0f;
		float SecondAlpha = 0.0f;
		if (IntersectSegments2D(
			PreviewState.SimplifiedPoints[PreviewState.FirstSegmentIndex],
			PreviewState.SimplifiedPoints[PreviewState.FirstSegmentIndex + 1],
			PreviewState.SimplifiedPoints[PreviewState.SecondSegmentIndex],
			PreviewState.SimplifiedPoints[PreviewState.SecondSegmentIndex + 1],
			FirstAlpha,
			SecondAlpha))
		{
			const FVector FirstLocation = FMath::Lerp(
				PreviewState.SimplifiedPoints[PreviewState.FirstSegmentIndex],
				PreviewState.SimplifiedPoints[PreviewState.FirstSegmentIndex + 1],
				FirstAlpha);
			const FVector SecondLocation = FMath::Lerp(
				PreviewState.SimplifiedPoints[PreviewState.SecondSegmentIndex],
				PreviewState.SimplifiedPoints[PreviewState.SecondSegmentIndex + 1],
				SecondAlpha);
			if (FMath::Abs(FirstLocation.Z - SecondLocation.Z)
				<= MaximumJunctionHeightDifference)
			{
				PreviewState.IntersectionPoints.AddUnique(
					(FirstLocation + SecondLocation) * 0.5f);
			}
		}
		++PreviewState.SecondSegmentIndex;
	}

	OutSimplifiedPoints = PreviewState.SimplifiedPoints;
	OutIntersectionPoints = PreviewState.IntersectionPoints;
	return PreviewState.bComplete;
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

void ARoadNetworkActor::FindLoadedManagedActors(
	TArray<AProceduralRoadActor*>& OutRoadActors,
	TArray<AProceduralRoadJunctionActor*>& OutJunctionActors) const
{
	OutRoadActors.Reset();
	OutJunctionActors.Reset();
	if (!GetWorld() || !NetworkId.IsValid())
	{
		return;
	}

	for (TActorIterator<AProceduralRoadActor> Iterator(GetWorld()); Iterator; ++Iterator)
	{
		if (Iterator->IsManagedByRoadNetwork(NetworkId))
		{
			OutRoadActors.Add(*Iterator);
		}
	}
	for (TActorIterator<AProceduralRoadJunctionActor> Iterator(GetWorld()); Iterator; ++Iterator)
	{
		if (Iterator->IsManagedByRoadNetwork(NetworkId))
		{
			OutJunctionActors.Add(*Iterator);
		}
	}
}

void ARoadNetworkActor::FindLoadedManagedLandscapeBrushes(
	TArray<ARoadNetworkLandscapeBrush*>& OutBrushes) const
{
	OutBrushes.Reset();
	if (!GetWorld() || !NetworkId.IsValid())
	{
		return;
	}

	for (TActorIterator<ARoadNetworkLandscapeBrush> Iterator(GetWorld()); Iterator; ++Iterator)
	{
		if (Iterator->IsManagedBy(NetworkId))
		{
			OutBrushes.Add(*Iterator);
		}
	}
	for (const FRoadGeneratedLandscapeBrush& GeneratedBrush : GeneratedLandscapeBrushes)
	{
		ARoadNetworkLandscapeBrush* Brush = GeneratedBrush.BrushActor.LoadSynchronous();
		if (Brush && Brush->IsManagedBy(NetworkId))
		{
			OutBrushes.AddUnique(Brush);
		}
	}
}

void ARoadNetworkActor::RebuildGeneratedActors()
{
	if (!GetWorld() || !NetworkId.IsValid())
	{
		return;
	}

	TArray<AProceduralRoadActor*> ExistingRoadActors;
	TArray<AProceduralRoadJunctionActor*> ExistingJunctionActors;
	FindLoadedManagedActors(ExistingRoadActors, ExistingJunctionActors);
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
		if (Junction && Junction->IsManagedByRoadNetwork(NetworkId))
		{
			ExistingJunctionActors.AddUnique(Junction);
		}
	}
	for (AProceduralRoadJunctionActor* Junction : ExistingJunctionActors)
	{
		if (Junction && !UsedJunctionActors.Contains(Junction))
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
	LandscapePaintStatus = TEXT("Landscape paint is out of date. Use Rebuild Paint in Road Painting mode.");
	MarkPackageDirty();
}

bool ARoadNetworkActor::RebuildLandscapeMaterialPaint()
{
	CancelLandscapeMaterialPaintRebuild();
	if (!GetWorld())
	{
		LandscapePaintStatus = TEXT("Landscape paint skipped: the network has no world.");
		return true;
	}

	LandscapePaintRebuildState = MakeShared<FLandscapePaintRebuildState>();
	FLandscapePaintRebuildState& State = *LandscapePaintRebuildState;
	FindLoadedManagedLandscapeBrushes(State.ExistingBrushes);
	TSet<ALandscape*> LandscapeSet;
	for (TActorIterator<ALandscapeProxy> Iterator(GetWorld()); Iterator; ++Iterator)
	{
		if (ALandscape* Landscape = Iterator->GetLandscapeActor())
		{
			LandscapeSet.Add(Landscape);
		}
	}
	State.Landscapes = LandscapeSet.Array();
	bLandscapePaintRebuildPending = true;
	const bool bComplete = ContinueLandscapeMaterialPaintRebuild();
	if (!bComplete)
	{
		ScheduleLandscapeMaterialPaintRebuild();
	}
	return bComplete;
}

bool ARoadNetworkActor::ContinueLandscapeMaterialPaintRebuild()
{
	if (!LandscapePaintRebuildState)
	{
		bLandscapePaintRebuildPending = false;
		return true;
	}

	FLandscapePaintRebuildState& State = *LandscapePaintRebuildState;
	int32 Iterations = 0;
	while (Iterations < RoadPainting::MaxIterationsPerFrame
		&& State.Phase != FLandscapePaintRebuildState::EPhase::Complete)
	{
		++Iterations;
		switch (State.Phase)
		{
		case FLandscapePaintRebuildState::EPhase::BuildPaintLayers:
		{
			if (State.LinkIndex >= Links.Num())
			{
				State.Phase = FLandscapePaintRebuildState::EPhase::SynchronizeLandscapes;
				continue;
			}
			if (!State.bCurrentLinkPrepared)
			{
				State.bCurrentLinkPrepared = true;
				State.bCurrentLinkPainted = false;
				State.LinkSampleIndex = 0;
				State.CurrentPaintGroupIndex = INDEX_NONE;
				State.CurrentPaintLayerIndex = INDEX_NONE;
				State.CurrentLinkSamples.Reset();

				const FRoadNetworkLink& Link = Links[State.LinkIndex];
				UClass* RoadClass = Link.RoadClass.LoadSynchronous();
				AProceduralRoadActor* RoadDefaults = RoadClass
					? RoadClass->GetDefaultObject<AProceduralRoadActor>()
					: nullptr;
				if (!RoadDefaults)
				{
					continue;
				}

				const FProceduralRoadLandscapePaintSettings& Settings =
					RoadDefaults->GetLandscapePaintSettings();
				if (!Settings.bEnabled)
				{
					continue;
				}

				ULandscapeLayerInfoObject* LayerInfo = Settings.LayerInfo.LoadSynchronous();
				if (!LayerInfo || Settings.EditLayerName.IsNone()
					|| Settings.PaintWidth <= 0.0f || Settings.PaintFalloff < 0.0f)
				{
					State.BrushMessages.Add(FString::Printf(
						TEXT("Road class '%s' has incomplete Landscape Paint defaults."),
						*GetNameSafe(RoadClass)));
					continue;
				}
				if (Settings.PaintWidth + Settings.PaintFalloff * 2.0f
					<= RoadDefaults->GetRoadWidth()
					&& !State.NarrowPaintProfileWarnings.Contains(RoadClass))
				{
					State.BrushMessages.Add(FString::Printf(
						TEXT("Road class '%s' has a %.0f cm total paint mask across a %.0f cm road; increase Paint Width or Falloff to expose a painted shoulder."),
						*GetNameSafe(RoadClass),
						Settings.PaintWidth + Settings.PaintFalloff * 2.0f,
						RoadDefaults->GetRoadWidth()));
					State.NarrowPaintProfileWarnings.Add(RoadClass);
				}

				State.CurrentPaintGroupIndex = State.BrushGroups.IndexOfByPredicate(
					[&Settings](const FLandscapePaintRebuildState::FBrushEditLayerGroup& Candidate)
					{
						return Candidate.EditLayerName == Settings.EditLayerName;
					});
				if (State.CurrentPaintGroupIndex == INDEX_NONE)
				{
					FLandscapePaintRebuildState::FBrushEditLayerGroup& BrushGroup =
						State.BrushGroups.AddDefaulted_GetRef();
					BrushGroup.EditLayerName = Settings.EditLayerName;
					State.CurrentPaintGroupIndex = State.BrushGroups.Num() - 1;
				}
				FLandscapePaintRebuildState::FBrushEditLayerGroup& BrushGroup =
					State.BrushGroups[State.CurrentPaintGroupIndex];
				State.CurrentPaintLayerIndex = BrushGroup.PaintLayers.IndexOfByPredicate(
					[LayerInfo](const FRoadLandscapeBrushLayer& Candidate)
					{
						return Candidate.WeightmapLayerName == LayerInfo->LayerName;
					});
				if (State.CurrentPaintLayerIndex == INDEX_NONE)
				{
					FRoadLandscapeBrushLayer& PaintLayer =
						BrushGroup.PaintLayers.AddDefaulted_GetRef();
					PaintLayer.WeightmapLayerName = LayerInfo->LayerName;
					State.CurrentPaintLayerIndex = BrushGroup.PaintLayers.Num() - 1;
				}
				State.CurrentCoreHalfWidth = Settings.PaintWidth * 0.5f;
				State.CurrentFalloff = Settings.PaintFalloff;
				SampleLinkPath(Link, State.CurrentLinkSamples);
				continue;
			}
			if (State.CurrentPaintGroupIndex != INDEX_NONE
				&& State.LinkSampleIndex + 1 < State.CurrentLinkSamples.Num())
			{
				FRoadLandscapeBrushLayer& PaintLayer = State.BrushGroups[
					State.CurrentPaintGroupIndex].PaintLayers[State.CurrentPaintLayerIndex];
				FRoadLandscapeBrushSegment& Segment = PaintLayer.Segments.AddDefaulted_GetRef();
				Segment.WorldStart = State.CurrentLinkSamples[State.LinkSampleIndex];
				Segment.WorldEnd = State.CurrentLinkSamples[State.LinkSampleIndex + 1];
				Segment.CoreHalfWidth = State.CurrentCoreHalfWidth;
				Segment.Falloff = State.CurrentFalloff;
				++State.LinkSampleIndex;
				State.bCurrentLinkPainted = true;
				continue;
			}
			if (State.bCurrentLinkPainted)
			{
				++State.PaintedLinkCount;
			}
			++State.LinkIndex;
			State.bCurrentLinkPrepared = false;
			continue;
		}

		case FLandscapePaintRebuildState::EPhase::SynchronizeLandscapes:
		{
			if (State.LandscapeIndex >= State.Landscapes.Num())
			{
				State.Phase = FLandscapePaintRebuildState::EPhase::CleanupBrushes;
				continue;
			}
			if (!State.bLandscapePrepared)
			{
				State.bLandscapePrepared = true;
				State.bLandscapeSkipped = false;
				State.BrushGroupIndex = 0;
				State.bGroupPrepared = false;
				ALandscape* Landscape = State.Landscapes[State.LandscapeIndex];
				if (!Landscape->HasLayersContent())
				{
					State.BrushMessages.Add(FString::Printf(
						TEXT("Landscape '%s' does not have Edit Layers enabled."),
						*Landscape->GetActorLabel()));
					State.bLandscapeSkipped = true;
				}
				else
				{
					State.LandscapeInfo = Landscape->GetLandscapeInfo();
					State.MaterialLayers = Landscape->GetLayersFromMaterial();
					State.bLandscapeSkipped = State.LandscapeInfo == nullptr;
				}
				continue;
			}
			if (State.bLandscapeSkipped)
			{
				++State.LandscapeIndex;
				State.bLandscapePrepared = false;
				continue;
			}
			if (State.BrushGroupIndex >= State.BrushGroups.Num())
			{
				++State.LandscapeIndex;
				State.bLandscapePrepared = false;
				continue;
			}
			if (!State.bGroupPrepared)
			{
				State.bGroupPrepared = true;
				State.PaintLayerIndex = 0;
				State.ExistingBrushIndex = 0;
				State.ValidPaintLayers.Reset();
				State.EditLayerIndex = INDEX_NONE;
				State.bEditLayerResolved = false;
				State.bGroupFailed = false;
				State.bBrushApplied = false;
				State.CurrentBrush = nullptr;
				continue;
			}

			FLandscapePaintRebuildState::FBrushEditLayerGroup& BrushGroup =
				State.BrushGroups[State.BrushGroupIndex];
			ALandscape* Landscape = State.Landscapes[State.LandscapeIndex];
			if (State.PaintLayerIndex < BrushGroup.PaintLayers.Num())
			{
				FRoadLandscapeBrushLayer& PaintLayer =
					BrushGroup.PaintLayers[State.PaintLayerIndex++];
				if (!State.MaterialLayers.Contains(PaintLayer.WeightmapLayerName))
				{
					State.BrushMessages.Add(FString::Printf(
						TEXT("Landscape material '%s' does not expose layer '%s'."),
						*GetNameSafe(Landscape->GetLandscapeMaterial()),
						*PaintLayer.WeightmapLayerName.ToString()));
				}
				else if (!State.LandscapeInfo->GetLayerInfoByName(PaintLayer.WeightmapLayerName))
				{
					State.BrushMessages.Add(FString::Printf(
						TEXT("Layer '%s' has no Layer Info assignment on Landscape '%s'."),
						*PaintLayer.WeightmapLayerName.ToString(),
						*Landscape->GetActorLabel()));
				}
				else
				{
					State.ValidPaintLayers.Add(PaintLayer);
				}
				continue;
			}
			if (State.ValidPaintLayers.IsEmpty())
			{
				++State.BrushGroupIndex;
				State.bGroupPrepared = false;
				continue;
			}
			if (!State.bEditLayerResolved)
			{
				State.bEditLayerResolved = true;
				State.EditLayerIndex = Landscape->GetLayerIndex(BrushGroup.EditLayerName);
				if (State.EditLayerIndex == INDEX_NONE
					&& Landscape->CanHaveLayersContent())
				{
					Landscape->Modify();
					Landscape->CreateLayer(BrushGroup.EditLayerName);
					State.EditLayerIndex = Landscape->GetLayerIndex(BrushGroup.EditLayerName);
				}
				if (State.EditLayerIndex == INDEX_NONE)
				{
					State.BrushMessages.Add(FString::Printf(
						TEXT("Landscape edit layer '%s' could not be created."),
						*BrushGroup.EditLayerName.ToString()));
					State.bGroupFailed = true;
				}
				continue;
			}
			if (State.bGroupFailed)
			{
				++State.BrushGroupIndex;
				State.bGroupPrepared = false;
				continue;
			}
			if (State.ExistingBrushIndex < State.ExistingBrushes.Num()
				&& !State.CurrentBrush)
			{
				ARoadNetworkLandscapeBrush* ExistingBrush =
					State.ExistingBrushes[State.ExistingBrushIndex++];
				if (ExistingBrush->GetOwningLandscape() == Landscape
					&& ExistingBrush->GetManagedEditLayerName() == BrushGroup.EditLayerName)
				{
					State.CurrentBrush = ExistingBrush;
				}
				continue;
			}
			if (!State.bBrushApplied)
			{
				State.bBrushApplied = true;
				if (!State.CurrentBrush)
				{
					FActorSpawnParameters SpawnParameters;
					SpawnParameters.ObjectFlags |= RF_Transactional;
					SpawnParameters.SpawnCollisionHandlingOverride =
						ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
					State.CurrentBrush = GetWorld()->SpawnActor<ARoadNetworkLandscapeBrush>(
						ARoadNetworkLandscapeBrush::StaticClass(),
						FTransform::Identity,
						SpawnParameters);
					if (State.CurrentBrush)
					{
						State.CurrentBrush->SetActorLabel(FString::Printf(
							TEXT("Road Landscape Brush - %s"),
							*BrushGroup.EditLayerName.ToString()));
					}
				}
				if (!State.CurrentBrush)
				{
					State.BrushMessages.Add(TEXT("Failed to create a managed road Landscape brush."));
					State.bGroupFailed = true;
					continue;
				}

				Landscape->Modify();
				const int32 CurrentBrushLayerIndex = Landscape->GetBrushLayer(State.CurrentBrush);
				if (CurrentBrushLayerIndex != State.EditLayerIndex)
				{
					if (CurrentBrushLayerIndex != INDEX_NONE)
					{
						Landscape->RemoveBrush(State.CurrentBrush);
					}
					Landscape->AddBrushToLayer(State.EditLayerIndex, State.CurrentBrush);
				}
				State.CurrentBrush->Configure(
					this,
					NetworkId,
					BrushGroup.EditLayerName,
					State.ValidPaintLayers);
				State.UsedBrushes.Add(State.CurrentBrush);
				FRoadGeneratedLandscapeBrush& GeneratedBrush =
					State.NewGeneratedBrushes.AddDefaulted_GetRef();
				GeneratedBrush.Landscape = Landscape;
				GeneratedBrush.EditLayerName = BrushGroup.EditLayerName;
				GeneratedBrush.BrushActor = State.CurrentBrush;
				continue;
			}

			++State.BrushGroupIndex;
			State.bGroupPrepared = false;
			continue;
		}

		case FLandscapePaintRebuildState::EPhase::CleanupBrushes:
		{
			if (State.CleanupBrushIndex >= State.ExistingBrushes.Num())
			{
				State.Phase = FLandscapePaintRebuildState::EPhase::Complete;
				continue;
			}
			ARoadNetworkLandscapeBrush* ExistingBrush =
				State.ExistingBrushes[State.CleanupBrushIndex++];
			if (!ExistingBrush || State.UsedBrushes.Contains(ExistingBrush))
			{
				continue;
			}
			if (ALandscape* Landscape = ExistingBrush->GetOwningLandscape())
			{
				Landscape->Modify();
				Landscape->RemoveBrush(ExistingBrush);
			}
			ExistingBrush->Destroy();
			continue;
		}

		case FLandscapePaintRebuildState::EPhase::Complete:
			break;
		}
	}

	if (State.Phase != FLandscapePaintRebuildState::EPhase::Complete)
	{
		return false;
	}

	GeneratedLandscapeBrushes = MoveTemp(State.NewGeneratedBrushes);
	if (!GeneratedLandscapeBrushes.IsEmpty())
	{
		State.BrushMessages.Insert(FString::Printf(
			TEXT("Landscape paint synchronized from %d road links through %d bounded edit-layer brushes."),
			State.PaintedLinkCount,
			GeneratedLandscapeBrushes.Num()), 0);
	}
	else if (State.BrushGroups.IsEmpty())
	{
		State.BrushMessages.Insert(
			TEXT("Landscape paint disabled in the road Blueprint defaults; managed brushes removed."),
			0);
	}
	LandscapePaintStatus = FString::Join(State.BrushMessages, TEXT(" "));
	MarkPackageDirty();
	RequestLandscapePaintUpdate();
	bLandscapePaintRebuildPending = false;
	LandscapePaintRebuildState.Reset();
	return true;
}

void ARoadNetworkActor::CancelLandscapeMaterialPaintRebuild()
{
	if (LandscapePaintRebuildTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(LandscapePaintRebuildTickerHandle);
		LandscapePaintRebuildTickerHandle.Reset();
	}
	LandscapePaintRebuildState.Reset();
	bLandscapePaintRebuildPending = false;
}

void ARoadNetworkActor::ScheduleLandscapeMaterialPaintRebuild()
{
	if (LandscapePaintRebuildTickerHandle.IsValid())
	{
		return;
	}

	LandscapePaintRebuildTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateWeakLambda(
			this,
			[this](float)
			{
				LandscapePaintRebuildTickerHandle.Reset();
				if (LandscapePaintRebuildState && !ContinueLandscapeMaterialPaintRebuild())
				{
					ScheduleLandscapeMaterialPaintRebuild();
				}
				return false;
			}),
		0.0f);
}

void ARoadNetworkActor::RequestLandscapePaintUpdate()
{
	ScheduleLandscapePaintUpdate();
}

void ARoadNetworkActor::CancelLandscapePaintUpdate()
{
	if (LandscapePaintUpdateTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(LandscapePaintUpdateTickerHandle);
		LandscapePaintUpdateTickerHandle.Reset();
	}
}

void ARoadNetworkActor::ScheduleLandscapePaintUpdate()
{
	if (LandscapePaintUpdateTickerHandle.IsValid())
	{
		return;
	}

	LandscapePaintUpdateTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateWeakLambda(
			this,
			[this](float)
			{
				LandscapePaintUpdateTickerHandle.Reset();
				TSet<ALandscape*> Landscapes;
				for (const FRoadGeneratedLandscapeBrush& GeneratedBrush :
					GeneratedLandscapeBrushes)
				{
					if (ARoadNetworkLandscapeBrush* Brush =
						GeneratedBrush.BrushActor.LoadSynchronous())
					{
						Brush->MarkTargetLayerComponentsDirty();
						if (ALandscape* Landscape = Brush->GetOwningLandscape())
						{
							Landscapes.Add(Landscape);
						}
					}
				}
				for (ALandscape* Landscape : Landscapes)
				{
					// Queue only the editing weightmap pass. The managed brush filters
					// RenderLayer_Native to its configured target weightmap layer.
					Landscape->RequestLayersContentUpdate(
						ELandscapeLayerUpdateMode::Update_Weightmap_Editing_NoCollision);
				}
				return false;
			}),
		0.0f);
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
	if (SelectedPointIds.Num() == 1 && SelectedLinkIds.IsEmpty())
	{
		return DeletePoint(SelectedPointIds[0]);
	}
	if (SelectedLinkIds.Num() == 1 && SelectedPointIds.IsEmpty())
	{
		return DeleteLink(SelectedLinkIds[0]);
	}
	if (SelectedPointIds.IsEmpty() && SelectedLinkIds.IsEmpty())
	{
		return false;
	}

	TSet<FGuid> PointIdsToDelete;
	for (const FGuid& PointId : SelectedPointIds)
	{
		PointIdsToDelete.Add(PointId);
	}
	TSet<FGuid> LinkIdsToDelete;
	for (const FGuid& LinkId : SelectedLinkIds)
	{
		LinkIdsToDelete.Add(LinkId);
	}
	Modify();
	bForceFullRebuild = true;
	Links.RemoveAll(
		[&PointIdsToDelete, &LinkIdsToDelete](const FRoadNetworkLink& Link)
		{
			return LinkIdsToDelete.Contains(Link.Id)
				|| PointIdsToDelete.Contains(Link.StartPointId)
				|| PointIdsToDelete.Contains(Link.EndPointId);
		});
	Points.RemoveAll(
		[&PointIdsToDelete](const FRoadNetworkPoint& Point)
		{
			return PointIdsToDelete.Contains(Point.Id);
		});
	RemoveIsolatedPoints();
	SetSelection(FGuid(), FGuid());
	RebuildGeneratedActors();
	return true;
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
	TSet<UClass*> ValidatedRoadClasses;
	for (const FRoadNetworkLink& Link : Links)
	{
		UClass* RoadClass = Link.RoadClass.LoadSynchronous();
		if (!RoadClass || ValidatedRoadClasses.Contains(RoadClass))
		{
			continue;
		}
		ValidatedRoadClasses.Add(RoadClass);
		AProceduralRoadActor* RoadDefaults = RoadClass->GetDefaultObject<AProceduralRoadActor>();
		if (!RoadDefaults)
		{
			continue;
		}

		FProceduralRoadLandscapePaintSettings Settings = RoadDefaults->GetLandscapePaintSettings();
		if (!Settings.bEnabled)
		{
			continue;
		}
		if (Settings.LayerInfo.IsNull())
		{
			OutErrors += FString::Printf(
				TEXT("Road class '%s' enables Landscape painting without a Landscape Layer Info asset.\n"),
				*GetNameSafe(RoadClass));
		}
		if (Settings.EditLayerName.IsNone())
		{
			OutErrors += FString::Printf(
				TEXT("Road class '%s' has no Landscape edit-layer name.\n"),
				*GetNameSafe(RoadClass));
		}
		if (Settings.PaintWidth <= 0.0f || Settings.PaintFalloff < 0.0f)
		{
			OutErrors += FString::Printf(
				TEXT("Road class '%s' has invalid Landscape paint width or falloff defaults.\n"),
				*GetNameSafe(RoadClass));
		}
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

void ARoadNetworkActor::RebuildLandscapePaint()
{
	RebuildLandscapeMaterialPaint();
}

void ARoadNetworkActor::AdoptSelected()
{
	const FScopedTransaction Transaction(NSLOCTEXT("RoadPainting", "AdoptRoads", "Adopt Roads"));
	AdoptSelectedRoads();
}

void ARoadNetworkActor::ValidateNetwork()
{
	FString Errors;
	if (ValidateNetworkGraph(Errors))
	{
		UE_LOG(LogRoadPainting, Display, TEXT("Road network %s is valid."), *GetActorLabel());
	}
	else
	{
		UE_LOG(LogRoadPainting, Error, TEXT("Road network %s is invalid:\n%s"), *GetActorLabel(), *Errors);
	}
}
