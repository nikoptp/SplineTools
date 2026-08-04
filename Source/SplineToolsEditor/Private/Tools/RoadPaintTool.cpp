#include "Tools/RoadPaintTool.h"

#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "InteractiveToolManager.h"
#include "LandscapeProxy.h"
#include "ProceduralRoadJunctionActor.h"
#include "RoadNetworkActor.h"
#include "RoadPaintingEditorMode.h"
#include "SceneManagement.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "RoadPaintTool"

namespace
{
	void SimplifyPreviewRange(
		const TArray<FVector>& Points,
		int32 FirstIndex,
		int32 LastIndex,
		float ToleranceSquared,
		TArray<bool>& KeepPoints)
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
				Points[PointIndex],
				Points[FirstIndex],
				Points[LastIndex]);
			const float DistanceSquared = FVector::DistSquared(Points[PointIndex], Closest);
			if (DistanceSquared > GreatestDistanceSquared)
			{
				GreatestDistanceSquared = DistanceSquared;
				GreatestIndex = PointIndex;
			}
		}
		if (GreatestIndex == INDEX_NONE || GreatestDistanceSquared <= ToleranceSquared)
		{
			return;
		}
		KeepPoints[GreatestIndex] = true;
		SimplifyPreviewRange(Points, FirstIndex, GreatestIndex, ToleranceSquared, KeepPoints);
		SimplifyPreviewRange(Points, GreatestIndex, LastIndex, ToleranceSquared, KeepPoints);
	}

	void SimplifyPreviewStroke(
		const TArray<FVector>& Points,
		float Tolerance,
		TArray<FVector>& OutPoints)
	{
		OutPoints.Reset();
		if (Points.Num() < 2)
		{
			OutPoints = Points;
			return;
		}
		TArray<bool> KeepPoints;
		KeepPoints.Init(false, Points.Num());
		KeepPoints[0] = true;
		KeepPoints.Last() = true;
		SimplifyPreviewRange(
			Points,
			0,
			Points.Num() - 1,
			FMath::Square(Tolerance),
			KeepPoints);
		for (int32 PointIndex = 0; PointIndex < Points.Num(); ++PointIndex)
		{
			if (KeepPoints[PointIndex])
			{
				OutPoints.Add(Points[PointIndex]);
			}
		}
	}
}

bool URoadPaintToolBuilder::CanBuildTool(const FToolBuilderState& SceneState) const
{
	return SceneState.World != nullptr;
}

UInteractiveTool* URoadPaintToolBuilder::BuildTool(
	const FToolBuilderState& SceneState) const
{
	URoadPaintTool* Tool = NewObject<URoadPaintTool>(SceneState.ToolManager);
	Tool->Initialize(SceneState.World, Cast<URoadPaintingEditorMode>(GetOuter()));
	return Tool;
}

void URoadPaintTool::Initialize(
	UWorld* World,
	URoadPaintingEditorMode* EditorMode)
{
	TargetWorld = World;
	Mode = EditorMode;
}

void URoadPaintTool::Setup()
{
	UInteractiveTool::Setup();
	UClickDragInputBehavior* MouseBehavior = NewObject<UClickDragInputBehavior>();
	MouseBehavior->Initialize(this);
	AddInputBehavior(MouseBehavior);

	Properties = NewObject<URoadPaintToolProperties>(this);
	if (ARoadNetworkActor* Network = FindNetwork())
	{
		Properties->RoadClass = Network->DefaultRoadClass.LoadSynchronous();
		Properties->JunctionClass = Network->JunctionClass.LoadSynchronous();
		Properties->SampleSpacing = Network->StrokeSampleSpacing;
		Properties->SimplificationTolerance = Network->SimplificationTolerance;
		Properties->SnapRadius = Network->SnapRadius;
		Properties->CrossingSampleInterval = Network->CrossingSampleInterval;
		Properties->IntersectionMergeRadius = Network->IntersectionMergeRadius;
		Properties->MaximumJunctionHeightDifference = Network->MaximumJunctionHeightDifference;
	}
	if (!Properties->RoadClass && GEditor)
	{
		for (FSelectionIterator Iterator(*GEditor->GetSelectedActors()); Iterator; ++Iterator)
		{
			if (AProceduralRoadActor* Road = Cast<AProceduralRoadActor>(*Iterator))
			{
				Properties->RoadClass = Road->GetClass();
				break;
			}
		}
	}
	if (!Properties->JunctionClass)
	{
		Properties->JunctionClass = AProceduralRoadJunctionActor::StaticClass();
	}
	AddToolPropertySource(Properties);
}

void URoadPaintTool::Shutdown(EToolShutdownType ShutdownType)
{
	StrokePoints.Reset();
	SimplifiedStrokePoints.Reset();
	PreviewIntersections.Reset();
	UInteractiveTool::Shutdown(ShutdownType);
}

ARoadNetworkActor* URoadPaintTool::FindNetwork() const
{
	return Mode ? Mode->FindRoadNetwork() : nullptr;
}

void URoadPaintTool::CopyPropertiesToNetwork(ARoadNetworkActor* Network) const
{
	Network->DefaultRoadClass = Properties->RoadClass;
	Network->JunctionClass = Properties->JunctionClass;
	Network->StrokeSampleSpacing = Properties->SampleSpacing;
	Network->SimplificationTolerance = Properties->SimplificationTolerance;
	Network->SnapRadius = Properties->SnapRadius;
	Network->CrossingSampleInterval = Properties->CrossingSampleInterval;
	Network->IntersectionMergeRadius = Properties->IntersectionMergeRadius;
	Network->MaximumJunctionHeightDifference = Properties->MaximumJunctionHeightDifference;
}

FInputRayHit URoadPaintTool::FindLandscapeHit(
	const FRay& WorldRay,
	FVector& OutLocation) const
{
	if (!TargetWorld)
	{
		return FInputRayHit();
	}
	TArray<FHitResult> Hits;
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(RoadPaintTool), true);
	TargetWorld->LineTraceMultiByChannel(
		Hits,
		WorldRay.Origin,
		WorldRay.PointAt(1000000.0f),
		ECC_Visibility,
		QueryParams);
	for (const FHitResult& Hit : Hits)
	{
		if (Hit.GetActor() && Hit.GetActor()->IsA<ALandscapeProxy>())
		{
			OutLocation = Hit.ImpactPoint;
			return FInputRayHit(Hit.Distance);
		}
	}
	return FInputRayHit();
}

FInputRayHit URoadPaintTool::CanBeginClickDragSequence(
	const FInputDeviceRay& PressPos)
{
	FVector HitLocation;
	return Properties && Properties->RoadClass
		? FindLandscapeHit(PressPos.WorldRay, HitLocation)
		: FInputRayHit();
}

void URoadPaintTool::AddStrokeSample(const FVector& WorldLocation, bool bForce)
{
	CursorLocation = WorldLocation;
	bHasCursor = true;
	if (StrokePoints.IsEmpty()
		|| bForce
		|| FVector::DistSquared(StrokePoints.Last(), WorldLocation)
			>= FMath::Square(Properties->SampleSpacing))
	{
		if (StrokePoints.IsEmpty() || !StrokePoints.Last().Equals(WorldLocation, 1.0f))
		{
			StrokePoints.Add(WorldLocation);
		}
	}
	UpdateStrokePreview();
}

void URoadPaintTool::UpdateStrokePreview()
{
	SimplifyPreviewStroke(
		StrokePoints,
		Properties->SimplificationTolerance,
		SimplifiedStrokePoints);
	PreviewIntersections.Reset();
	bHasSnapTarget = false;
	if (ARoadNetworkActor* Network = FindNetwork())
	{
		TArray<FVector> NetworkSimplifiedPoints;
		Network->BuildStrokePreview(
			StrokePoints,
			NetworkSimplifiedPoints,
			PreviewIntersections);
		bHasSnapTarget = Network->FindSnapPreviewTarget(
			CursorLocation,
			SnapTargetLocation);
	}
}

void URoadPaintTool::OnClickPress(const FInputDeviceRay& PressPos)
{
	FVector HitLocation;
	if (FindLandscapeHit(PressPos.WorldRay, HitLocation).bHit)
	{
		StrokePoints.Reset();
		bDragging = true;
		AddStrokeSample(HitLocation, true);
	}
}

void URoadPaintTool::OnClickDrag(const FInputDeviceRay& DragPos)
{
	FVector HitLocation;
	if (bDragging && FindLandscapeHit(DragPos.WorldRay, HitLocation).bHit)
	{
		AddStrokeSample(HitLocation, false);
	}
}

void URoadPaintTool::OnClickRelease(const FInputDeviceRay& ReleasePos)
{
	FVector HitLocation;
	if (bDragging && FindLandscapeHit(ReleasePos.WorldRay, HitLocation).bHit)
	{
		AddStrokeSample(HitLocation, true);
	}
	bDragging = false;
	if (StrokePoints.Num() >= 2 && Mode && Properties->RoadClass)
	{
		const FScopedTransaction Transaction(LOCTEXT("PaintRoadStroke", "Paint Road Stroke"));
		if (ARoadNetworkActor* Network = Mode->GetOrCreateRoadNetwork())
		{
			CopyPropertiesToNetwork(Network);
			Network->AddPaintedStroke(StrokePoints, Properties->RoadClass);
		}
	}
	StrokePoints.Reset();
	SimplifiedStrokePoints.Reset();
	PreviewIntersections.Reset();
	bHasSnapTarget = false;
}

void URoadPaintTool::OnTerminateDragSequence()
{
	bDragging = false;
	StrokePoints.Reset();
	SimplifiedStrokePoints.Reset();
	PreviewIntersections.Reset();
	bHasSnapTarget = false;
}

void URoadPaintTool::OnUpdateModifierState(int ModifierID, bool bIsOn)
{
}

void URoadPaintTool::Render(IToolsContextRenderAPI* RenderAPI)
{
	FPrimitiveDrawInterface* PDI = RenderAPI->GetPrimitiveDrawInterface();
	if (ARoadNetworkActor* Network = FindNetwork())
	{
		for (const FRoadNetworkLink& Link : Network->GetLinks())
		{
			const FRoadNetworkPoint* StartPoint = Network->FindPoint(Link.StartPointId);
			const FRoadNetworkPoint* EndPoint = Network->FindPoint(Link.EndPointId);
			if (StartPoint && EndPoint)
			{
				PDI->DrawLine(StartPoint->WorldLocation, EndPoint->WorldLocation, FColor(70, 180, 255), SDPG_Foreground, 2.0f);
			}
		}
		for (const FRoadNetworkPoint& Point : Network->GetPoints())
		{
			PDI->DrawPoint(Point.WorldLocation, FColor::Cyan, 8.0f, SDPG_Foreground);
		}
	}
	for (int32 PointIndex = 0; PointIndex + 1 < StrokePoints.Num(); ++PointIndex)
	{
		PDI->DrawLine(StrokePoints[PointIndex], StrokePoints[PointIndex + 1], FColor(255, 180, 0), SDPG_Foreground, 2.0f);
	}
	for (const FVector& Point : StrokePoints)
	{
		PDI->DrawPoint(Point, FColor::Yellow, 10.0f, SDPG_Foreground);
	}
	for (int32 PointIndex = 0; PointIndex + 1 < SimplifiedStrokePoints.Num(); ++PointIndex)
	{
		PDI->DrawLine(
			SimplifiedStrokePoints[PointIndex],
			SimplifiedStrokePoints[PointIndex + 1],
			FColor::Green,
			SDPG_Foreground,
			4.0f);
	}
	for (const FVector& Intersection : PreviewIntersections)
	{
		PDI->DrawPoint(Intersection, FColor::Magenta, 18.0f, SDPG_Foreground);
	}
	if (bHasSnapTarget)
	{
		PDI->DrawPoint(SnapTargetLocation, FColor::Yellow, 18.0f, SDPG_Foreground);
		PDI->DrawLine(CursorLocation, SnapTargetLocation, FColor::Yellow, SDPG_Foreground, 1.0f);
	}
	if (bHasCursor)
	{
		DrawWireSphere(PDI, CursorLocation, FColor::White, Properties->SnapRadius, 32, SDPG_Foreground, 1.0f);
	}
}

#undef LOCTEXT_NAMESPACE
