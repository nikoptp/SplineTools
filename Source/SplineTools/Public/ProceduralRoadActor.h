#pragma once

#include "CoreMinimal.h"
#include "Components/SplineComponent.h"
#include "SplineToolActorBase.h"
#include "ProceduralRoadActor.generated.h"

class UDecalComponent;
class ULandscapeLayerInfoObject;
class UMaterialInterface;
class UProceduralMeshComponent;
struct FSplineRoadCrossSection;

UENUM(BlueprintType)
enum class ERoadSplineEndpoint : uint8
{
	Start,
	End,
};

USTRUCT(BlueprintType)
struct SPLINETOOLS_API FProceduralRoadSplinePoint
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road")
	FVector WorldLocation = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road")
	FVector WorldArriveTangent = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road")
	FVector WorldLeaveTangent = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road")
	FRotator Rotation = FRotator::ZeroRotator;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road")
	FVector Scale = FVector::OneVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road")
	TEnumAsByte<ESplinePointType::Type> Type = ESplinePointType::Curve;
};

/** Exact generated geometry at a road end used to form a gap-free junction seam. */
USTRUCT(BlueprintType)
struct SPLINETOOLS_API FProceduralRoadJunctionEdgeGeometry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Road")
	TArray<FVector> SurfacePoints;

	UPROPERTY(BlueprintReadOnly, Category = "Road")
	FVector LeftFlapPoint = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "Road")
	FVector RightFlapPoint = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "Road")
	FVector Direction = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "Road")
	bool bHasSideFlaps = false;

	UPROPERTY(BlueprintReadOnly, Category = "Road")
	bool bUsesCachedMesh = false;
};

/** Editor-authored Landscape material paint generated for this road class. */
USTRUCT(BlueprintType)
struct SPLINETOOLS_API FProceduralRoadLandscapePaintSettings
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Landscape Paint")
	bool bEnabled = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Landscape Paint", meta = (EditCondition = "bEnabled"))
	TSoftObjectPtr<ULandscapeLayerInfoObject> LayerInfo;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Landscape Paint", meta = (EditCondition = "bEnabled"))
	FName EditLayerName = TEXT("RoadPainting");

	/** Total fully-painted width in world units before falloff begins. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Landscape Paint", meta = (EditCondition = "bEnabled", ClampMin = "1.0"))
	float PaintWidth = 1000.0f;

	/** Additional fade distance on each side in world units. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Landscape Paint", meta = (EditCondition = "bEnabled", ClampMin = "0.0"))
	float PaintFalloff = 300.0f;
};

UCLASS(BlueprintType)
class SPLINETOOLS_API AProceduralRoadActor : public ASplineToolActorBase
{
	GENERATED_BODY()

public:
	AProceduralRoadActor();

	virtual void RebuildSplineTool() override;

	/** Rebuilds and serializes the road cache. Save the actor/level after running it. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Road", meta = (DisplayName = "Bake Road Cache"))
	void RebuildRoad();

	UFUNCTION(BlueprintPure, Category = "Road")
	bool HasCachedRoadData() const;

	UFUNCTION(BlueprintPure, Category = "Road|Material")
	UMaterialInterface* GetRoadMaterial() const;

	UFUNCTION(BlueprintPure, Category = "Road|Material")
	UMaterialInterface* GetRoadSideFlapMaterial() const;

	UFUNCTION(BlueprintPure, Category = "Road")
	float GetRoadWidth() const;

	const FProceduralRoadLandscapePaintSettings& GetLandscapePaintSettings() const;

	bool SetJunctionTrim(
		ERoadSplineEndpoint Endpoint,
		float TrimDistance,
		AActor* JunctionOwner);
	void ClearJunctionTrim(ERoadSplineEndpoint Endpoint, AActor* JunctionOwner);
	bool GetJunctionEdge(
		ERoadSplineEndpoint Endpoint,
		float TrimDistance,
		FVector& OutCenter,
		FVector& OutLeft,
		FVector& OutRight,
		FVector& OutDirection) const;
	bool GetJunctionEdgeSamples(
		ERoadSplineEndpoint Endpoint,
		float TrimDistance,
		TArray<FVector>& OutEdgePoints,
		FVector& OutDirection) const;
	bool GetJunctionEdgeGeometry(
		ERoadSplineEndpoint Endpoint,
		float TrimDistance,
		FProceduralRoadJunctionEdgeGeometry& OutGeometry) const;
	float GetRoadSplineLength() const;
	bool GetJunctionTrimDistance(
		ERoadSplineEndpoint Endpoint,
		float& OutTrimDistance) const;
	bool GetSplineEndpointLocation(
		ERoadSplineEndpoint Endpoint,
		FVector& OutLocation) const;
	bool IsJunctionEndpointAvailable(
		ERoadSplineEndpoint Endpoint,
		const AActor* JunctionOwner) const;

#if WITH_EDITOR
	virtual void PostEditUndo() override;

	void GetRoadSplinePoints(TArray<FProceduralRoadSplinePoint>& OutPoints) const;
	void SetRoadSplinePoints(
		const TArray<FProceduralRoadSplinePoint>& Points,
		bool bInClosedLoop,
		bool bRebuild = true);
	void SampleRoadSplineSegment(
		int32 SegmentIndex,
		float SampleInterval,
		TArray<FVector>& OutWorldPoints) const;
	bool IsRoadSplineClosedLoop() const;
	void SetManagedRoadIdentity(const FGuid& NetworkId, const FGuid& RunId);
	bool IsManagedByRoadNetwork(const FGuid& NetworkId) const;
	FGuid GetManagedRoadRunId() const;
	void SetEditorRebuildDeferred(bool bDeferred);
#endif

protected:
	virtual void PostLoad() override;
	virtual void PostDuplicate(EDuplicateMode::Type DuplicateMode) override;
	virtual void PostInitializeComponents() override;
	virtual void UpdateSplineSettings() override;
	virtual void ResetGeneratedContent() override;
	virtual bool ShouldRebuildInGameWorld() const override;

private:
	void RestoreCachedRoadComponents();
	void UpdateRoadChunks();
	void EnsureRoadChunkCount(int32 RequiredChunkCount);
	UProceduralMeshComponent* CreateRoadChunk(int32 ChunkIndex);
	void ConfigureRoadChunk(UProceduralMeshComponent* RoadChunk) const;
	void RebuildRoadChunk(
		UProceduralMeshComponent* RoadChunk,
		const TArray<FSplineRoadCrossSection>& CrossSections);
	uint32 CalculateChunkSourceHash(float StartDistance, float EndDistance) const;
	void BuildCrossSections(
		float StartDistance,
		float EndDistance,
		TArray<FSplineRoadCrossSection>& OutCrossSections) const;
	void BuildRoadSurface(
		UProceduralMeshComponent* RoadChunk,
		const TArray<FSplineRoadCrossSection>& CrossSections);
	void BuildSideFlaps(
		UProceduralMeshComponent* RoadChunk,
		const TArray<FSplineRoadCrossSection>& CrossSections);
	void BuildRoadLines(
		UProceduralMeshComponent* RoadChunk,
		const TArray<FSplineRoadCrossSection>& CrossSections);
	void BuildSimpleCollision(
		UProceduralMeshComponent* RoadChunk,
		const TArray<FSplineRoadCrossSection>& CrossSections);
	void ResetGeneratedDecals();
	void GenerateDecals();
	FVector SampleRoadPosition(
		float DistanceAlongSpline,
		float LateralOffset,
		FVector* OutSurfaceNormal = nullptr) const;
	bool TraceTerrain(const FVector& DesiredPosition, FHitResult& OutHit) const;
	void CreateMeshSection(
		UProceduralMeshComponent* RoadChunk,
		int32 SectionIndex,
		const TArray<FVector>& Vertices,
		const TArray<int32>& Triangles,
		const TArray<FVector2D>& UVs,
		const TArray<FVector2D>& MarkingUVs,
		UMaterialInterface* Material);
	UMaterialInterface* GetCenterLineMaterial() const;
	float GetEffectiveStartDistance() const;
	float GetEffectiveEndDistance() const;

private:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Road", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UProceduralMeshComponent> RoadMesh;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road", meta = (ClampMin = "10.0", AllowPrivateAccess = "true"))
	float RoadWidth = 600.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road", meta = (ClampMin = "10.0", AllowPrivateAccess = "true"))
	float SegmentLength = 200.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road", meta = (ClampMin = "100.0", AllowPrivateAccess = "true"))
	float ChunkLength = 5000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road", meta = (ClampMin = "1", ClampMax = "16", AllowPrivateAccess = "true"))
	int32 WidthSubdivisions = 2;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road", meta = (AllowPrivateAccess = "true"))
	bool bClosedLoop = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Material", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UMaterialInterface> RoadMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Material", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UMaterialInterface> SideFlapMaterial;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Road|Landscape Paint", meta = (AllowPrivateAccess = "true", ShowOnlyInnerProperties))
	FProceduralRoadLandscapePaintSettings LandscapePaintSettings;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Material", meta = (ClampMin = "1.0", AllowPrivateAccess = "true"))
	FVector2D UVWorldSize = FVector2D(400.0f, 400.0f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Material", meta = (ClampMin = "1.0", AllowPrivateAccess = "true"))
	float MarkingUVWorldLength = 400.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Lines|Side", meta = (DisplayName = "Side Line Material", AllowPrivateAccess = "true"))
	TObjectPtr<UMaterialInterface> RoadLineMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Lines|Center", meta = (AllowPrivateAccess = "true"))
	bool bGenerateCenterLine = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Lines|Side", meta = (AllowPrivateAccess = "true"))
	bool bGenerateSideLines = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Lines|Side", meta = (DisplayName = "Side Line Width", ClampMin = "1.0", AllowPrivateAccess = "true"))
	float RoadLineWidth = 12.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Lines|Side", meta = (ClampMin = "0.0", AllowPrivateAccess = "true"))
	float SideLineInset = 30.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Lines|Side", meta = (DisplayName = "Side Line Surface Offset", ClampMin = "0.0", AllowPrivateAccess = "true"))
	float RoadLineSurfaceOffset = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Lines|Side", meta = (DisplayName = "Side Line UV World Length", ClampMin = "1.0", AllowPrivateAccess = "true"))
	float RoadLineUVWorldLength = 100.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Lines|Center", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UMaterialInterface> CenterLineMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Lines|Center", meta = (ClampMin = "1.0", AllowPrivateAccess = "true"))
	float CenterLineWidth = 12.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Lines|Center", meta = (ClampMin = "0.0", AllowPrivateAccess = "true"))
	float CenterLineSurfaceOffset = 0.6f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Lines|Center", meta = (ClampMin = "1.0", AllowPrivateAccess = "true"))
	float CenterLineUVWorldLength = 100.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Lines|Center", meta = (AllowPrivateAccess = "true"))
	bool bCenterLineHasGaps = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Lines|Center", meta = (EditCondition = "bCenterLineHasGaps", ClampMin = "1.0", AllowPrivateAccess = "true"))
	float CenterLineDashLength = 300.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Lines|Center", meta = (EditCondition = "bCenterLineHasGaps", ClampMin = "1.0", AllowPrivateAccess = "true"))
	float CenterLineGapLength = 300.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Terrain", meta = (AllowPrivateAccess = "true"))
	bool bAlignToTerrain = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Terrain", meta = (ClampMin = "0.0", AllowPrivateAccess = "true"))
	float TerrainTraceHeightAbove = 1000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Terrain", meta = (ClampMin = "0.0", AllowPrivateAccess = "true"))
	float TerrainTraceHeightBelow = 3000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Terrain", meta = (AllowPrivateAccess = "true"))
	TEnumAsByte<ECollisionChannel> TerrainTraceChannel = ECC_Visibility;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Terrain", meta = (AllowPrivateAccess = "true"))
	float RoadSurfaceOffset = 3.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Terrain", meta = (ClampMin = "0.0", AllowPrivateAccess = "true"))
	float SideFlapWidth = 100.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Terrain", meta = (ClampMin = "0.0", AllowPrivateAccess = "true"))
	float SideFlapEmbedDepth = 15.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Terrain", meta = (AllowPrivateAccess = "true"))
	bool bGenerateEndFlaps = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Terrain", meta = (EditCondition = "bGenerateEndFlaps", ClampMin = "0.0", AllowPrivateAccess = "true"))
	float EndFlapLength = 100.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Terrain", meta = (EditCondition = "bGenerateEndFlaps", ClampMin = "0.0", AllowPrivateAccess = "true"))
	float EndFlapEmbedDepth = 15.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Collision", meta = (AllowPrivateAccess = "true"))
	bool bGenerateCollision = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Collision", meta = (ClampMin = "1.0", AllowPrivateAccess = "true"))
	float CollisionThickness = 20.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Collision", meta = (ClampMin = "1", ClampMax = "16", AllowPrivateAccess = "true"))
	int32 CollisionSegmentStride = 2;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Decals", meta = (AllowPrivateAccess = "true"))
	bool bGenerateDecals = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Decals", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UMaterialInterface> DecalMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Decals", meta = (ClampMin = "1.0", AllowPrivateAccess = "true"))
	float DecalSpacing = 400.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Decals", meta = (ClampMin = "0.0", AllowPrivateAccess = "true"))
	float DecalStartPadding = 100.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Decals", meta = (ClampMin = "0.0", AllowPrivateAccess = "true"))
	float DecalEndPadding = 100.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Decals", meta = (AllowPrivateAccess = "true"))
	float DecalLateralOffset = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Decals", meta = (AllowPrivateAccess = "true"))
	FVector DecalSize = FVector(50.0f, 100.0f, 100.0f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Decals", meta = (AllowPrivateAccess = "true"))
	FRotator DecalRotationOffset = FRotator::ZeroRotator;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Decals", meta = (ClampMin = "0.0", ClampMax = "1.0", AllowPrivateAccess = "true"))
	float DecalFadeScreenSize = 0.01f;

	/** Serialized editor-built decal cache loaded directly by PIE and packaged builds. */
	UPROPERTY()
	TArray<TObjectPtr<UDecalComponent>> GeneratedDecals;

	/** Serialized editor-built mesh cache loaded directly by PIE and packaged builds. */
	UPROPERTY()
	TArray<TObjectPtr<UProceduralMeshComponent>> GeneratedRoadChunks;

	/** Source hashes persisted with the mesh cache for selective editor rebuilds. */
	UPROPERTY()
	TArray<uint32> RoadChunkSourceHashes;
	TWeakObjectPtr<AActor> StartJunctionOwner;
	TWeakObjectPtr<AActor> EndJunctionOwner;
	float StartJunctionTrimDistance = 0.0f;
	float EndJunctionTrimDistance = 0.0f;

#if WITH_EDITORONLY_DATA
	UPROPERTY(VisibleAnywhere, Category = "Road|Painting")
	FGuid ManagedRoadNetworkId;

	UPROPERTY(VisibleAnywhere, Category = "Road|Painting")
	FGuid ManagedRoadRunId;

	UPROPERTY(Transient)
	bool bEditorRebuildDeferred = false;
#endif
};
