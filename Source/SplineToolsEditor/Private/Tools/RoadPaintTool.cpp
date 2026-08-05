#include "Tools/RoadPaintTool.h"

#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "InteractiveToolManager.h"
#include "ProceduralRoadJunctionActor.h"
#include "RoadNetworkActor.h"
#include "RoadPaintingLandscapeQuery.h"
#include "RoadPaintingEditorMode.h"
#include "SceneManagement.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "RoadPaintTool"

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
	UMouseHoverBehavior* HoverBehavior = NewObject<UMouseHoverBehavior>();
	HoverBehavior->Initialize(this);
	AddInputBehavior(HoverBehavior);
	UMouseWheelInputBehavior* WheelBehavior = NewObject<UMouseWheelInputBehavior>();
	WheelBehavior->Initialize(this);
	WheelBehavior->ModifierCheckFunc = [](const FInputDeviceState& InputState)
	{
		return FInputDeviceState::IsShiftKeyDown(InputState);
	};
	AddInputBehavior(WheelBehavior);

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
	if (!Properties->RoadClass)
	{
		Properties->RoadClass = AProceduralRoadActor::StaticClass();
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
	StrokePreviewState.Reset();
	bStrokePreviewPending = false;
	UInteractiveTool::Shutdown(ShutdownType);
}

void URoadPaintTool::OnTick(float)
{
	if (!bStrokePreviewPending)
	{
		return;
	}

	ARoadNetworkActor* Network = FindNetwork();
	if (!Network)
	{
		StrokePreviewState.Reset();
		PreviewIntersections.Reset();
		bStrokePreviewPending = false;
		return;
	}

	bStrokePreviewPending = !Network->BuildStrokePreview(
		StrokePoints,
		StrokePreviewState,
		SimplifiedStrokePoints,
		PreviewIntersections);
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
	float HitDistance = 0.0f;
	if (RoadPaintingLandscapeQuery::FindSurfaceAlongSegment(
		TargetWorld,
		WorldRay.Origin,
		WorldRay.PointAt(1000000.0f),
		OutLocation,
		HitDistance))
	{
		return FInputRayHit(HitDistance);
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
	if (StrokePreviewState.InputPoints != StrokePoints)
	{
		StrokePreviewState.Reset();
		SimplifiedStrokePoints.Reset();
		PreviewIntersections.Reset();
	}
	bStrokePreviewPending = true;
	bHasSnapTarget = false;
	if (!FindNetwork())
	{
		StrokePreviewState.Reset();
		bStrokePreviewPending = false;
		return;
	}
	ARoadNetworkActor* Network = FindNetwork();
	bHasSnapTarget = Network->FindSnapPreviewTarget(
		CursorLocation,
		SnapTargetLocation);
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
	StrokePreviewState.Reset();
	bHasSnapTarget = false;
	bStrokePreviewPending = false;
}

void URoadPaintTool::OnTerminateDragSequence()
{
	bDragging = false;
	StrokePoints.Reset();
	SimplifiedStrokePoints.Reset();
	PreviewIntersections.Reset();
	StrokePreviewState.Reset();
	bHasSnapTarget = false;
	bStrokePreviewPending = false;
}

void URoadPaintTool::OnUpdateModifierState(int ModifierID, bool bIsOn)
{
}

FInputRayHit URoadPaintTool::BeginHoverSequenceHitTest(
	const FInputDeviceRay& PressPos)
{
	FVector HitLocation;
	return Properties && Properties->RoadClass
		? FindLandscapeHit(PressPos.WorldRay, HitLocation)
		: FInputRayHit();
}

void URoadPaintTool::OnBeginHover(const FInputDeviceRay& DevicePos)
{
	OnUpdateHover(DevicePos);
}

bool URoadPaintTool::OnUpdateHover(const FInputDeviceRay& DevicePos)
{
	FVector HitLocation;
	if (!FindLandscapeHit(DevicePos.WorldRay, HitLocation).bHit)
	{
		OnEndHover();
		return false;
	}

	CursorLocation = HitLocation;
	bHasCursor = true;
	UpdateStrokePreview();
	return true;
}

void URoadPaintTool::OnEndHover()
{
	bHasCursor = false;
	bHasSnapTarget = false;
}

FInputRayHit URoadPaintTool::ShouldRespondToMouseWheel(
	const FInputDeviceRay& CurrentPos)
{
	FVector HitLocation;
	return Properties && FindLandscapeHit(CurrentPos.WorldRay, HitLocation).bHit
		? FInputRayHit(1.0f)
		: FInputRayHit();
}

void URoadPaintTool::OnMouseWheelScrollUp(const FInputDeviceRay& CurrentPos)
{
	Properties->SnapRadius = FMath::Clamp(Properties->SnapRadius + 50.0f, 10.0f, 10000.0f);
	OnUpdateHover(CurrentPos);
}

void URoadPaintTool::OnMouseWheelScrollDown(const FInputDeviceRay& CurrentPos)
{
	Properties->SnapRadius = FMath::Clamp(Properties->SnapRadius - 50.0f, 10.0f, 10000.0f);
	OnUpdateHover(CurrentPos);
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
