#pragma once

#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProceduralRoadActor.h"
#include "ProceduralRoadJunctionActor.generated.h"

class UMaterialInterface;
class UProceduralMeshComponent;
class USceneComponent;
class USphereComponent;

USTRUCT(BlueprintType)
struct FProceduralRoadJunctionConnection
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Junction")
	TObjectPtr<AProceduralRoadActor> Road;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Junction")
	ERoadSplineEndpoint Endpoint = ERoadSplineEndpoint::End;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Junction", meta = (ClampMin = "0.0"))
	float TrimDistance = 300.0f;
};

UCLASS(BlueprintType)
class SPLINETOOLS_API AProceduralRoadJunctionActor : public AActor
{
	GENERATED_BODY()

public:
	AProceduralRoadJunctionActor();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Road Junction")
	void RebuildJunction();

#if WITH_EDITOR
	void SetManagedConnections(
		const TArray<FProceduralRoadJunctionConnection>& InConnections,
		const FGuid& NetworkId,
		const FGuid& NodeId,
		bool bRebuild = true);
	bool IsManagedByRoadNetwork(const FGuid& NetworkId) const;
	FGuid GetManagedRoadNodeId() const;
	const TArray<FProceduralRoadJunctionConnection>& GetRoadConnections() const;
#endif

#if WITH_EDITOR
	static void NotifyRoadEdited(AProceduralRoadActor* Road);
#endif

protected:
	virtual void BeginPlay() override;
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void Destroyed() override;

#if WITH_EDITOR
	virtual void PostEditMove(bool bFinished) override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void PostEditUndo() override;
#endif

private:
	struct FAppliedConnection
	{
		TWeakObjectPtr<AProceduralRoadActor> Road;
		ERoadSplineEndpoint Endpoint = ERoadSplineEndpoint::End;
	};

	void DiscoverNearbyRoadEndpoints();
	void SynchronizeRoadTrims();
	void ReleaseRoadTrims();
	bool IsConnectionConfigured(const FAppliedConnection& AppliedConnection) const;
	UMaterialInterface* GetEffectiveJunctionMaterial() const;
	bool GenerateJunctionPatch();
	void UpdateEditorVisualization(bool bHasGeneratedMesh);
	bool TraceTerrain(const FVector& DesiredPosition, FHitResult& OutHit) const;
	FVector ProjectToTerrain(const FVector& DesiredPosition) const;

#if WITH_EDITOR
	void QueueEditorRebuild();
	void CancelQueuedEditorRebuild();
#endif

private:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Road Junction", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Road Junction", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UProceduralMeshComponent> JunctionMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Road Junction", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USphereComponent> DiscoveryPreview;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road Junction|Discovery", meta = (AllowPrivateAccess = "true"))
	bool bAutoDiscoverRoadEndpoints = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road Junction|Discovery", meta = (ClampMin = "1.0", AllowPrivateAccess = "true"))
	float EndpointSearchRadius = 1500.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road Junction|Discovery", meta = (ClampMin = "0.0", AllowPrivateAccess = "true"))
	float AutomaticTrimDistance = 300.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road Junction", meta = (EditCondition = "!bAutoDiscoverRoadEndpoints", AllowPrivateAccess = "true"))
	TArray<FProceduralRoadJunctionConnection> Connections;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road Junction|Material", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UMaterialInterface> JunctionMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road Junction|Material", meta = (ClampMin = "1.0", AllowPrivateAccess = "true"))
	FVector2D UVWorldSize = FVector2D(400.0f, 400.0f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road Junction|Terrain", meta = (AllowPrivateAccess = "true"))
	bool bAlignToTerrain = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road Junction|Terrain", meta = (ClampMin = "0.0", AllowPrivateAccess = "true"))
	float TerrainTraceHeightAbove = 1000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road Junction|Terrain", meta = (ClampMin = "0.0", AllowPrivateAccess = "true"))
	float TerrainTraceHeightBelow = 3000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road Junction|Terrain", meta = (AllowPrivateAccess = "true"))
	TEnumAsByte<ECollisionChannel> TerrainTraceChannel = ECC_Visibility;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road Junction|Terrain", meta = (AllowPrivateAccess = "true"))
	float SurfaceOffset = 3.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road Junction|Collision", meta = (AllowPrivateAccess = "true"))
	bool bGenerateCollision = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Road Junction|Collision", meta = (ClampMin = "1.0", AllowPrivateAccess = "true"))
	float CollisionThickness = 20.0f;

	TArray<FAppliedConnection> AppliedConnections;

#if WITH_EDITORONLY_DATA
	UPROPERTY(VisibleAnywhere, Category = "Road Junction|Painting")
	FGuid ManagedRoadNetworkId;

	UPROPERTY(VisibleAnywhere, Category = "Road Junction|Painting")
	FGuid ManagedRoadNodeId;
#endif

#if WITH_EDITOR
	FTSTicker::FDelegateHandle EditorRebuildTickerHandle;
#endif
};
