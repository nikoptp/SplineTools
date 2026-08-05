#pragma once

#include "CoreMinimal.h"
#include "LandscapeBlueprintBrushBase.h"
#include "RoadNetworkLandscapeBrush.generated.h"

class ARoadNetworkActor;
class UTextureRenderTarget2D;

USTRUCT()
struct SPLINETOOLSEDITOR_API FRoadLandscapeBrushSegment
{
	GENERATED_BODY()

	UPROPERTY()
	FVector WorldStart = FVector::ZeroVector;

	UPROPERTY()
	FVector WorldEnd = FVector::ZeroVector;

	UPROPERTY()
	float CoreHalfWidth = 0.0f;

	UPROPERTY()
	float Falloff = 0.0f;
};

USTRUCT()
struct SPLINETOOLSEDITOR_API FRoadLandscapeBrushLayer
{
	GENERATED_BODY()

	UPROPERTY()
	FName WeightmapLayerName;

	UPROPERTY()
	TArray<FRoadLandscapeBrushSegment> Segments;
};

/** Editor-only Landscape edit-layer brush managed by a road network. */
UCLASS(NotBlueprintable, NotPlaceable)
class SPLINETOOLSEDITOR_API ARoadNetworkLandscapeBrush : public ALandscapeBlueprintBrushBase
{
	GENERATED_BODY()

public:
	ARoadNetworkLandscapeBrush(const FObjectInitializer& ObjectInitializer);

	virtual bool IsEditorOnly() const override;
	virtual void Initialize_Native(
		const FTransform& InLandscapeTransform,
		const FIntPoint& InLandscapeSize,
		const FIntPoint& InLandscapeRenderTargetSize) override;
	virtual UTextureRenderTarget2D* RenderLayer_Native(
		const FLandscapeBrushParameters& InParameters) override;
	virtual bool AffectsWeightmapLayer(const FName& InLayerName) const override;

	void Configure(
		ARoadNetworkActor* InRoadNetwork,
		const FGuid& InNetworkId,
		FName InEditLayerName,
		const TArray<FRoadLandscapeBrushLayer>& InPaintLayers);
	void MarkTargetLayerComponentsDirty();
	bool IsManagedBy(const FGuid& InNetworkId) const;
	FName GetManagedEditLayerName() const;

private:
	void EnsureOutputRenderTarget(const UTextureRenderTarget2D* SourceRenderTarget);

	UPROPERTY()
	TSoftObjectPtr<ARoadNetworkActor> RoadNetwork;

	UPROPERTY()
	FGuid NetworkId;

	UPROPERTY()
	FName EditLayerName;

	UPROPERTY()
	TArray<FRoadLandscapeBrushLayer> PaintLayers;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> OutputRenderTarget;

	FTransform LandscapeTransform = FTransform::Identity;
	FIntPoint LandscapeSize = FIntPoint::ZeroValue;
	FIntPoint LandscapeRenderTargetSize = FIntPoint::ZeroValue;
};
