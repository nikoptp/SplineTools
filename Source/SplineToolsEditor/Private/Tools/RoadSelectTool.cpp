#include "Tools/RoadSelectTool.h"

#include "Engine/World.h"
#include "InteractiveToolManager.h"
#include "RoadNetworkActor.h"
#include "RoadPaintingLandscapeQuery.h"
#include "RoadPaintingEditorMode.h"
#include "SceneManagement.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "RoadSelectTool"

namespace
{
	float DistanceSquaredToSegment2D(
		const FVector& Point,
		const FVector& Start,
		const FVector& End)
	{
		const FVector2D Segment(End.X - Start.X, End.Y - Start.Y);
		const FVector2D Offset(Point.X - Start.X, Point.Y - Start.Y);
		const float LengthSquared = Segment.SizeSquared();
		const float Alpha = LengthSquared > KINDA_SMALL_NUMBER
			? FMath::Clamp(FVector2D::DotProduct(Offset, Segment) / LengthSquared, 0.0f, 1.0f)
			: 0.0f;
		return FVector2D::DistSquared(
			FVector2D(Point.X, Point.Y),
			FVector2D(Start.X + Segment.X * Alpha, Start.Y + Segment.Y * Alpha));
	}
}

bool URoadSelectToolBuilder::CanBuildTool(const FToolBuilderState& SceneState) const
{
	return SceneState.World != nullptr;
}

UInteractiveTool* URoadSelectToolBuilder::BuildTool(
	const FToolBuilderState& SceneState) const
{
	URoadSelectTool* Tool = NewObject<URoadSelectTool>(SceneState.ToolManager);
	Tool->Initialize(SceneState.World, Cast<URoadPaintingEditorMode>(GetOuter()));
	return Tool;
}

void URoadSelectTool::Initialize(
	UWorld* World,
	URoadPaintingEditorMode* EditorMode)
{
	TargetWorld = World;
	Mode = EditorMode;
}

void URoadSelectTool::Setup()
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
	Properties = NewObject<URoadSelectToolProperties>(this);
	AddToolPropertySource(Properties);
}

ARoadNetworkActor* URoadSelectTool::FindNetwork() const
{
	return Mode ? Mode->FindRoadNetwork() : nullptr;
}

FInputRayHit URoadSelectTool::FindLandscapeHit(
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

FInputRayHit URoadSelectTool::CanBeginClickDragSequence(
	const FInputDeviceRay& PressPos)
{
	FVector HitLocation;
	return FindNetwork() ? FindLandscapeHit(PressPos.WorldRay, HitLocation) : FInputRayHit();
}

void URoadSelectTool::SelectAtLocation(const FVector& WorldLocation)
{
	ARoadNetworkActor* Network = FindNetwork();
	if (!Network)
	{
		return;
	}
	FGuid BestPointId;
	FGuid BestLinkId;
	FindClosestElement(WorldLocation, BestPointId, BestLinkId);
	Network->SetSelection(BestPointId, BestLinkId);
}

void URoadSelectTool::FindClosestElement(
	const FVector& WorldLocation,
	FGuid& OutPointId,
	FGuid& OutLinkId) const
{
	OutPointId.Invalidate();
	OutLinkId.Invalidate();
	ARoadNetworkActor* Network = FindNetwork();
	if (!Network)
	{
		return;
	}

	const float RadiusSquared = FMath::Square(Properties->SelectionRadius);
	float BestDistanceSquared = RadiusSquared;
	for (const FRoadNetworkPoint& Point : Network->GetPoints())
	{
		const float DistanceSquared = FVector2D::DistSquared(
			FVector2D(WorldLocation.X, WorldLocation.Y),
			FVector2D(Point.WorldLocation.X, Point.WorldLocation.Y));
		if (DistanceSquared < BestDistanceSquared)
		{
			BestDistanceSquared = DistanceSquared;
			OutPointId = Point.Id;
		}
	}
	if (OutPointId.IsValid())
	{
		return;
	}

	for (const FRoadNetworkLink& Link : Network->GetLinks())
	{
		TArray<FVector> LinkSamples;
		Network->GetLinkWorldSamples(Link, LinkSamples);
		for (int32 SampleIndex = 0; SampleIndex + 1 < LinkSamples.Num(); ++SampleIndex)
		{
			const float DistanceSquared = DistanceSquaredToSegment2D(
				WorldLocation,
				LinkSamples[SampleIndex],
				LinkSamples[SampleIndex + 1]);
			if (DistanceSquared < BestDistanceSquared)
			{
				BestDistanceSquared = DistanceSquared;
				OutLinkId = Link.Id;
			}
		}
	}
}

void URoadSelectTool::OnClickPress(const FInputDeviceRay& PressPos)
{
	FVector HitLocation;
	if (!FindLandscapeHit(PressPos.WorldRay, HitLocation).bHit)
	{
		return;
	}
	SelectAtLocation(HitLocation);
	if (ARoadNetworkActor* Network = FindNetwork())
	{
		if (Network->GetSelectedPointId().IsValid())
		{
			MoveTransaction = MakeUnique<FScopedTransaction>(
				LOCTEXT("MoveRoadPoint", "Move Road Point"));
			Network->Modify();
			bMovingPoint = true;
		}
	}
}

void URoadSelectTool::OnClickDrag(const FInputDeviceRay& DragPos)
{
	ARoadNetworkActor* Network = FindNetwork();
	FVector HitLocation;
	if (bMovingPoint && Network
		&& FindLandscapeHit(DragPos.WorldRay, HitLocation).bHit)
	{
		Network->MovePointToLandscape(Network->GetSelectedPointId(), HitLocation);
	}
}

void URoadSelectTool::OnClickRelease(const FInputDeviceRay& ReleasePos)
{
	if (bMovingPoint)
	{
		if (ARoadNetworkActor* Network = FindNetwork())
		{
			Network->RebuildDirty();
		}
	}
	bMovingPoint = false;
	MoveTransaction.Reset();
}

void URoadSelectTool::OnTerminateDragSequence()
{
	if (bMovingPoint)
	{
		if (ARoadNetworkActor* Network = FindNetwork())
		{
			Network->RebuildDirty();
		}
	}
	bMovingPoint = false;
	MoveTransaction.Reset();
}

void URoadSelectTool::OnUpdateModifierState(int ModifierID, bool bIsOn)
{
}

FInputRayHit URoadSelectTool::BeginHoverSequenceHitTest(
	const FInputDeviceRay& PressPos)
{
	FVector HitLocation;
	return FindNetwork()
		? FindLandscapeHit(PressPos.WorldRay, HitLocation)
		: FInputRayHit();
}

void URoadSelectTool::OnBeginHover(const FInputDeviceRay& DevicePos)
{
	OnUpdateHover(DevicePos);
}

bool URoadSelectTool::OnUpdateHover(const FInputDeviceRay& DevicePos)
{
	FVector HitLocation;
	if (!FindLandscapeHit(DevicePos.WorldRay, HitLocation).bHit)
	{
		OnEndHover();
		return false;
	}

	CursorLocation = HitLocation;
	bHasCursor = true;
	FindClosestElement(CursorLocation, HoverPointId, HoverLinkId);
	return true;
}

void URoadSelectTool::OnEndHover()
{
	bHasCursor = false;
	HoverPointId.Invalidate();
	HoverLinkId.Invalidate();
}

FInputRayHit URoadSelectTool::ShouldRespondToMouseWheel(
	const FInputDeviceRay& CurrentPos)
{
	FVector HitLocation;
	return FindLandscapeHit(CurrentPos.WorldRay, HitLocation).bHit
		? FInputRayHit(1.0f)
		: FInputRayHit();
}

void URoadSelectTool::OnMouseWheelScrollUp(const FInputDeviceRay& CurrentPos)
{
	Properties->SelectionRadius = FMath::Clamp(Properties->SelectionRadius + 50.0f, 10.0f, 10000.0f);
	OnUpdateHover(CurrentPos);
}

void URoadSelectTool::OnMouseWheelScrollDown(const FInputDeviceRay& CurrentPos)
{
	Properties->SelectionRadius = FMath::Clamp(Properties->SelectionRadius - 50.0f, 10.0f, 10000.0f);
	OnUpdateHover(CurrentPos);
}

void URoadSelectTool::Render(IToolsContextRenderAPI* RenderAPI)
{
	ARoadNetworkActor* Network = FindNetwork();
	if (!Network)
	{
		return;
	}
	FPrimitiveDrawInterface* PDI = RenderAPI->GetPrimitiveDrawInterface();
	for (const FRoadNetworkLink& Link : Network->GetLinks())
	{
		TArray<FVector> LinkSamples;
		Network->GetLinkWorldSamples(Link, LinkSamples);
		const bool bSelected = Link.Id == Network->GetSelectedLinkId();
		const bool bHovered = Link.Id == HoverLinkId;
		for (int32 SampleIndex = 0; SampleIndex + 1 < LinkSamples.Num(); ++SampleIndex)
		{
			PDI->DrawLine(
				LinkSamples[SampleIndex],
				LinkSamples[SampleIndex + 1],
				bSelected ? FColor::Yellow : bHovered ? FColor::Orange : FColor(70, 180, 255),
				SDPG_Foreground,
				bSelected ? 6.0f : bHovered ? 4.0f : 2.0f);
		}
	}
	for (const FRoadNetworkPoint& Point : Network->GetPoints())
	{
		const bool bSelected = Point.Id == Network->GetSelectedPointId();
		const bool bHovered = Point.Id == HoverPointId;
		PDI->DrawPoint(
			Point.WorldLocation,
			bSelected ? FColor::Yellow : bHovered ? FColor::Orange : FColor::Cyan,
			bSelected ? 18.0f : bHovered ? 15.0f : 10.0f,
			SDPG_Foreground);
	}
	if (bHasCursor)
	{
		DrawWireSphere(
			PDI,
			CursorLocation,
			HoverPointId.IsValid() || HoverLinkId.IsValid() ? FColor::Orange : FColor::White,
			Properties->SelectionRadius,
			24,
			SDPG_Foreground,
			1.0f);
		PDI->DrawPoint(CursorLocation, FColor::White, 8.0f, SDPG_Foreground);
	}
}

#undef LOCTEXT_NAMESPACE
