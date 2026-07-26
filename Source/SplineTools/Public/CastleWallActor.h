#pragma once

#include "CoreMinimal.h"
#include "SplineToolActorBase.h"
#include "CastleWallActor.generated.h"

class UStaticMesh;

UCLASS(BlueprintType)
class SPLINETOOLS_API ACastleWallActor : public ASplineToolActorBase
{
	GENERATED_BODY()

public:
	ACastleWallActor();

protected:
	virtual void UpdateSplineSettings() override;
	virtual void GenerateSplineContent() override;

private:
	FTransform BuildCastlePieceTransform(float DistanceAlongSpline, const FVector& InstanceScale, const FVector& LocalOffset) const;
	void CollectTowerDistances(TArray<float>& OutTowerDistances) const;
	void AddEvenlyDistributedTowerDistances(TArray<float>& OutTowerDistances) const;
	bool ShouldPlaceTowerAtPoint(int32 PointIndex) const;
	bool HasTowerNearDistance(float DistanceAlongSpline, const TArray<float>& TowerDistances, float Radius) const;
	UStaticMesh* PickWallMesh(FRandomStream& RandomStream, UStaticMesh* PreviousWallMesh) const;
	UStaticMesh* PickMesh(const TArray<TObjectPtr<UStaticMesh>>& MeshOptions, FRandomStream& RandomStream) const;

private:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Castle Wall", meta = (AllowPrivateAccess = "true"))
	TArray<TObjectPtr<UStaticMesh>> WallMeshes;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Castle Wall", meta = (ClampMin = "2", AllowPrivateAccess = "true"))
	int32 MinWallSegmentCount = 2;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Castle Wall", meta = (ClampMin = "10.0", AllowPrivateAccess = "true"))
	float WallSegmentSpacing = 300.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Castle Wall", meta = (AllowPrivateAccess = "true"))
	FVector WallScale = FVector(1.0f, 1.0f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Castle Wall", meta = (AllowPrivateAccess = "true"))
	FVector WallOffset = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Castle Wall", meta = (AllowPrivateAccess = "true"))
	bool bClosedLoop = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Castle Wall", meta = (AllowPrivateAccess = "true"))
	int32 RandomSeed = 1337;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Castle Wall|Terrain", meta = (AllowPrivateAccess = "true"))
	bool bSnapToTerrain = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Castle Wall|Terrain", meta = (ClampMin = "0.0", AllowPrivateAccess = "true"))
	float TerrainTraceHeightAbove = 500.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Castle Wall|Terrain", meta = (ClampMin = "0.0", AllowPrivateAccess = "true"))
	float TerrainTraceHeightBelow = 2000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Castle Wall|Towers", meta = (AllowPrivateAccess = "true"))
	TArray<TObjectPtr<UStaticMesh>> TowerMeshes;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Castle Wall|Towers", meta = (AllowPrivateAccess = "true"))
	FVector TowerScale = FVector(1.0f, 1.0f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Castle Wall|Towers", meta = (AllowPrivateAccess = "true"))
	FVector TowerOffset = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Castle Wall|Towers", meta = (ClampMin = "0.0", ClampMax = "180.0", AllowPrivateAccess = "true"))
	float CornerTurnAngleThresholdDegrees = 25.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Castle Wall|Towers", meta = (ClampMin = "0.0", AllowPrivateAccess = "true"))
	float TowerExclusionRadius = 175.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Castle Wall|Towers", meta = (ClampMin = "0", AllowPrivateAccess = "true"))
	int32 TowerAmount = 0;
};
