#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SplineToolActorBase.generated.h"

class UHierarchicalInstancedStaticMeshComponent;
class USceneComponent;
class USplineComponent;
class UStaticMesh;

UCLASS(Abstract, BlueprintType)
class SPLINETOOLS_API ASplineToolActorBase : public AActor
{
	GENERATED_BODY()

public:
	ASplineToolActorBase();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Spline Tool")
	virtual void RebuildSplineTool();

protected:
	virtual void BeginPlay() override;
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void UpdateSplineSettings();
	virtual void ResetGeneratedContent();
	virtual void GenerateSplineContent();
	virtual void FinalizeGeneratedContent();

	UHierarchicalInstancedStaticMeshComponent* CreateGeneratedHISM(const FString& ComponentName, UStaticMesh* StaticMesh);
	bool IsSplineUsable() const;
	FTransform GetSplineTransformAtDistance(float DistanceAlongSpline) const;
	FTransform BuildSplineInstanceTransform(
		float DistanceAlongSpline,
		const FVector& InstanceScale,
		const FVector& LocationOffset = FVector::ZeroVector,
		const FRotator& RotationOffset = FRotator::ZeroRotator
	) const;
	void AddSplineInstance(
		UHierarchicalInstancedStaticMeshComponent* MeshComponent,
		float DistanceAlongSpline,
		const FVector& InstanceScale,
		const FVector& LocationOffset = FVector::ZeroVector,
		const FRotator& RotationOffset = FRotator::ZeroRotator
	) const;

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Spline Tool")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Spline Tool")
	TObjectPtr<USplineComponent> ToolSpline;

private:
	UPROPERTY(Transient)
	TArray<TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> GeneratedInstanceComponents;

#if WITH_EDITOR
public:
	virtual void PostEditMove(bool bFinished) override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;

private:
	void QueueEditorRebuild();

	bool bEditorRebuildQueued = false;
#endif
};
