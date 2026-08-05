#include "RoadNetworkLandscapeBrush.h"

#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Landscape.h"
#include "LandscapeComponent.h"
#include "LandscapeInfo.h"
#include "RoadNetworkActor.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(RoadNetworkLandscapeBrush)

ARoadNetworkLandscapeBrush::ARoadNetworkLandscapeBrush(
	const FObjectInitializer& ObjectInitializer)
	: ALandscapeBlueprintBrushBase(ObjectInitializer)
{
	SetCanAffectHeightmap(false);
	SetCanAffectWeightmap(true);
	SetCanAffectVisibilityLayer(false);
}

bool ARoadNetworkLandscapeBrush::IsEditorOnly() const
{
	return true;
}

void ARoadNetworkLandscapeBrush::Initialize_Native(
	const FTransform& InLandscapeTransform,
	const FIntPoint& InLandscapeSize,
	const FIntPoint& InLandscapeRenderTargetSize)
{
	LandscapeTransform = InLandscapeTransform;
	LandscapeSize = InLandscapeSize;
	LandscapeRenderTargetSize = InLandscapeRenderTargetSize;
}

void ARoadNetworkLandscapeBrush::Configure(
	ARoadNetworkActor* InRoadNetwork,
	const FGuid& InNetworkId,
	FName InEditLayerName,
	const TArray<FRoadLandscapeBrushLayer>& InPaintLayers)
{
	Modify();
	RoadNetwork = InRoadNetwork;
	NetworkId = InNetworkId;
	EditLayerName = InEditLayerName;
	PaintLayers = InPaintLayers;
	AffectedWeightmapLayers.Reset(PaintLayers.Num());
	for (const FRoadLandscapeBrushLayer& PaintLayer : PaintLayers)
	{
		AffectedWeightmapLayers.AddUnique(PaintLayer.WeightmapLayerName);
	}
	MarkPackageDirty();
}

void ARoadNetworkLandscapeBrush::MarkTargetLayerComponentsDirty()
{
	ALandscape* Landscape = GetOwningLandscape();
	ULandscapeInfo* LandscapeInfo = Landscape ? Landscape->GetLandscapeInfo() : nullptr;
	if (!LandscapeInfo)
	{
		return;
	}

	const FVector LandscapeScale = Landscape->GetTransform().GetScale3D().GetAbs();
	const float RadiusScaleX = LandscapeScale.X > KINDA_SMALL_NUMBER
		? 1.0f / LandscapeScale.X
		: 1.0f;
	const float RadiusScaleY = LandscapeScale.Y > KINDA_SMALL_NUMBER
		? 1.0f / LandscapeScale.Y
		: 1.0f;
	const FTransform WorldToLandscape = Landscape->GetTransform().Inverse();
	FBox2D TargetBounds(EForceInit::ForceInit);
	for (const FRoadLandscapeBrushLayer& PaintLayer : PaintLayers)
	{
		for (const FRoadLandscapeBrushSegment& Segment : PaintLayer.Segments)
		{
			const FVector LocalStart = WorldToLandscape.TransformPosition(Segment.WorldStart);
			const FVector LocalEnd = WorldToLandscape.TransformPosition(Segment.WorldEnd);
			const float Radius = Segment.CoreHalfWidth + Segment.Falloff;
			const FVector2D RadiusOffset(Radius * RadiusScaleX, Radius * RadiusScaleY);
			TargetBounds += FVector2D(LocalStart.X, LocalStart.Y) - RadiusOffset;
			TargetBounds += FVector2D(LocalStart.X, LocalStart.Y) + RadiusOffset;
			TargetBounds += FVector2D(LocalEnd.X, LocalEnd.Y) - RadiusOffset;
			TargetBounds += FVector2D(LocalEnd.X, LocalEnd.Y) + RadiusOffset;
		}
	}

	if (!TargetBounds.bIsValid)
	{
		return;
	}

	TSet<ULandscapeComponent*> TargetComponents;
	LandscapeInfo->GetComponentsInRegion(
		FMath::FloorToInt(TargetBounds.Min.X),
		FMath::FloorToInt(TargetBounds.Min.Y),
		FMath::CeilToInt(TargetBounds.Max.X),
		FMath::CeilToInt(TargetBounds.Max.Y),
		TargetComponents,
		true);
	for (ULandscapeComponent* Component : TargetComponents)
	{
		if (Component)
		{
			Component->RequestWeightmapUpdate(false, false);
		}
	}
}

bool ARoadNetworkLandscapeBrush::IsManagedBy(const FGuid& InNetworkId) const
{
	return NetworkId == InNetworkId;
}

FName ARoadNetworkLandscapeBrush::GetManagedEditLayerName() const
{
	return EditLayerName;
}

bool ARoadNetworkLandscapeBrush::AffectsWeightmapLayer(const FName& InLayerName) const
{
	return PaintLayers.ContainsByPredicate(
		[InLayerName](const FRoadLandscapeBrushLayer& PaintLayer)
		{
			return PaintLayer.WeightmapLayerName == InLayerName
				&& !PaintLayer.Segments.IsEmpty();
		});
}

void ARoadNetworkLandscapeBrush::EnsureOutputRenderTarget(
	const UTextureRenderTarget2D* SourceRenderTarget)
{
	if (!SourceRenderTarget)
	{
		return;
	}

	const bool bNeedsResource = !OutputRenderTarget
		|| OutputRenderTarget->SizeX != SourceRenderTarget->SizeX
		|| OutputRenderTarget->SizeY != SourceRenderTarget->SizeY
		|| OutputRenderTarget->RenderTargetFormat != SourceRenderTarget->RenderTargetFormat;
	if (!bNeedsResource)
	{
		return;
	}

	OutputRenderTarget = NewObject<UTextureRenderTarget2D>(this, NAME_None, RF_Transient);
	OutputRenderTarget->RenderTargetFormat = SourceRenderTarget->RenderTargetFormat;
	OutputRenderTarget->ClearColor = FLinearColor::Black;
	OutputRenderTarget->AddressX = TA_Clamp;
	OutputRenderTarget->AddressY = TA_Clamp;
	OutputRenderTarget->bAutoGenerateMips = false;
	OutputRenderTarget->InitAutoFormat(SourceRenderTarget->SizeX, SourceRenderTarget->SizeY);
	OutputRenderTarget->UpdateResourceImmediate(true);
}

UTextureRenderTarget2D* ARoadNetworkLandscapeBrush::RenderLayer_Native(
	const FLandscapeBrushParameters& InParameters)
{
	if (InParameters.LayerType != ELandscapeToolTargetType::Weightmap
		|| !InParameters.CombinedResult
		|| !GetWorld()
		|| !AffectsWeightmapLayer(InParameters.WeightmapLayerName))
	{
		return InParameters.CombinedResult;
	}

	const FRoadLandscapeBrushLayer* PaintLayer = PaintLayers.FindByPredicate(
		[&InParameters](const FRoadLandscapeBrushLayer& Candidate)
		{
			return Candidate.WeightmapLayerName == InParameters.WeightmapLayerName;
		});
	if (!PaintLayer || LandscapeSize.X <= 0 || LandscapeSize.Y <= 0)
	{
		return InParameters.CombinedResult;
	}

	EnsureOutputRenderTarget(InParameters.CombinedResult);
	if (!OutputRenderTarget || !OutputRenderTarget->GameThread_GetRenderTargetResource())
	{
		return InParameters.CombinedResult;
	}

	FCanvas Canvas(
		OutputRenderTarget->GameThread_GetRenderTargetResource(),
		nullptr,
		GetWorld(),
		GetWorld()->GetFeatureLevel(),
		FCanvas::CDM_ImmediateDrawing);
	FCanvasTileItem SourceTile(
		FVector2D::ZeroVector,
		InParameters.CombinedResult->GetResource(),
		FVector2D(OutputRenderTarget->SizeX, OutputRenderTarget->SizeY),
		FLinearColor::White);
	SourceTile.BlendMode = SE_BLEND_AlphaBlend;
	Canvas.DrawItem(SourceTile);

	const FVector LandscapeScale = LandscapeTransform.GetScale3D().GetAbs();
	const float PixelScaleX = LandscapeScale.X > KINDA_SMALL_NUMBER
		? 1.0f / LandscapeScale.X
		: 1.0f;
	const float PixelScaleY = LandscapeScale.Y > KINDA_SMALL_NUMBER
		? 1.0f / LandscapeScale.Y
		: 1.0f;
	const float RadiusPixelScale = FMath::Max(PixelScaleX, PixelScaleY);

	constexpr int32 FalloffBandCount = 8;
	for (const FRoadLandscapeBrushSegment& Segment : PaintLayer->Segments)
	{
		FVector LocalStart = LandscapeTransform.InverseTransformPosition(Segment.WorldStart);
		FVector LocalEnd = LandscapeTransform.InverseTransformPosition(Segment.WorldEnd);
		FVector2D Start(LocalStart.X + 0.5f, LocalStart.Y + 0.5f);
		FVector2D End(LocalEnd.X + 0.5f, LocalEnd.Y + 0.5f);
		FVector2D Direction = End - Start;
		const float SegmentLength = Direction.Size();
		if (SegmentLength <= KINDA_SMALL_NUMBER)
		{
			continue;
		}

		const float CoreRadius = FMath::Max(0.5f, Segment.CoreHalfWidth * RadiusPixelScale);
		const float FalloffRadius = FMath::Max(0.0f, Segment.Falloff * RadiusPixelScale);
		const float Angle = FMath::RadiansToDegrees(FMath::Atan2(Direction.Y, Direction.X));
		const FVector2D Midpoint = (Start + End) * 0.5f;
		for (int32 BandIndex = FalloffBandCount; BandIndex >= 0; --BandIndex)
		{
			const float TargetOpacity = 1.0f
				- static_cast<float>(BandIndex) / FalloffBandCount;
			const float PreviousOpacity = BandIndex == FalloffBandCount
				? 0.0f
				: 1.0f - static_cast<float>(BandIndex + 1) / FalloffBandCount;
			const float BandRadius = CoreRadius
				+ FalloffRadius * static_cast<float>(BandIndex) / FalloffBandCount;
			const float BlendOpacity = TargetOpacity >= 1.0f
				? 1.0f
				: (TargetOpacity - PreviousOpacity)
				/ FMath::Max(1.0f - PreviousOpacity, KINDA_SMALL_NUMBER);
			if (BlendOpacity <= 0.0f)
			{
				continue;
			}

			const FVector2D TileSize(
				SegmentLength + BandRadius * 2.0f,
				BandRadius * 2.0f);
			FCanvasTileItem RoadTile(
				Midpoint - TileSize * 0.5f,
				TileSize,
				FLinearColor(1.0f, 1.0f, 1.0f, BlendOpacity));
			RoadTile.BlendMode = SE_BLEND_Opaque;
			RoadTile.Rotation = FRotator(0.0f, Angle, 0.0f);
			RoadTile.PivotPoint = FVector2D(0.5f, 0.5f);
			Canvas.DrawItem(RoadTile);
		}
	}

	Canvas.Flush_GameThread();
	return OutputRenderTarget;
}
