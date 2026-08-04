#include "Tools/RoadSelectTool.h"

#include "Engine/World.h"
#include "InteractiveToolManager.h"
#include "LandscapeProxy.h"
#include "RoadNetworkActor.h"
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
	TArray<FHitResult> Hits;
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(RoadSelectTool), true);
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
	const float RadiusSquared = FMath::Square(Properties->SelectionRadius);
	float BestDistanceSquared = RadiusSquared;
	FGuid BestPointId;
	for (const FRoadNetworkPoint& Point : Network->GetPoints())
	{
		const float DistanceSquared = FVector2D::DistSquared(
			FVector2D(WorldLocation.X, WorldLocation.Y),
			FVector2D(Point.WorldLocation.X, Point.WorldLocation.Y));
		if (DistanceSquared < BestDistanceSquared)
		{
			BestDistanceSquared = DistanceSquared;
			BestPointId = Point.Id;
		}
	}
	if (BestPointId.IsValid())
	{
		Network->SetSelection(BestPointId, FGuid());
		return;
	}

	FGuid BestLinkId;
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
				BestLinkId = Link.Id;
			}
		}
	}
	Network->SetSelection(FGuid(), BestLinkId);
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
		for (int32 SampleIndex = 0; SampleIndex + 1 < LinkSamples.Num(); ++SampleIndex)
		{
			PDI->DrawLine(
				LinkSamples[SampleIndex],
				LinkSamples[SampleIndex + 1],
				bSelected ? FColor::Yellow : FColor(70, 180, 255),
				SDPG_Foreground,
				bSelected ? 6.0f : 2.0f);
		}
	}
	for (const FRoadNetworkPoint& Point : Network->GetPoints())
	{
		const bool bSelected = Point.Id == Network->GetSelectedPointId();
		PDI->DrawPoint(
			Point.WorldLocation,
			bSelected ? FColor::Yellow : FColor::Cyan,
			bSelected ? 18.0f : 10.0f,
			SDPG_Foreground);
	}
}

#undef LOCTEXT_NAMESPACE
