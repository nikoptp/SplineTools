#pragma once

#include "CoreMinimal.h"
#include "SplineToolActorBase.h"
#include "ProceduralRoadActor.generated.h"

class UDecalComponent;
class UMaterialInterface;
class UProceduralMeshComponent;
struct FSplineRoadCrossSection;

UCLASS(BlueprintType)
class SPLINETOOLS_API AProceduralRoadActor : public ASplineToolActorBase
{
	GENERATED_BODY()

public:
	AProceduralRoadActor();

	virtual void RebuildSplineTool() override;

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Road")
	void RebuildRoad();

protected:
	virtual void UpdateSplineSettings() override;
	virtual void ResetGeneratedContent() override;

private:
	void UpdateRoadChunks();
	void EnsureRoadChunkCount(int32 RequiredChunkCount);
	UProceduralMeshComponent* CreateRoadChunk(int32 ChunkIndex);
	void ConfigureRoadChunk(UProceduralMeshComponent* RoadChunk) const;
	void RebuildRoadChunk(
		UProceduralMeshComponent* RoadChunk,
		const TArray<FSplineRoadCrossSection>& CrossSections);
	uint32 CalculateChunkHash(const TArray<FSplineRoadCrossSection>& CrossSections) const;
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
		UMaterialInterface* Material);

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

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road|Material", meta = (ClampMin = "1.0", AllowPrivateAccess = "true"))
	FVector2D UVWorldSize = FVector2D(400.0f, 400.0f);

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

	UPROPERTY(Transient)
	TArray<TObjectPtr<UDecalComponent>> GeneratedDecals;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UProceduralMeshComponent>> GeneratedRoadChunks;

	TArray<uint32> RoadChunkHashes;
};
