#pragma once

#include "CoreMinimal.h"
#include "SplineToolActorBase.h"
#include "RopeBridge.generated.h"

class UPhysicsConstraintComponent;
class USplineComponent;
class UStaticMesh;
class UStaticMeshComponent;

UCLASS(BlueprintType)
class SPLINETOOLS_API ARopeBridge : public ASplineToolActorBase
{
	GENERATED_BODY()

public:
	ARopeBridge();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Bridge")
	void RebuildBridge();

protected:
	virtual void ResetGeneratedContent() override;
	virtual void GenerateSplineContent() override;

private:
	void CreateBridgePart(int32 PartIndex, int32 PartCount, float DistanceAlongSpline, FRandomStream& RandomStream);
	void CreateConstraint(int32 ConstraintIndex);
	UStaticMesh* PickBridgeMesh(FRandomStream& RandomStream) const;
	bool IsAnchoredPart(int32 PartIndex, int32 PartCount) const;

public:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Bridge")
	TObjectPtr<USplineComponent> BridgeSpline;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Bridge")
	TArray<TObjectPtr<UStaticMesh>> BridgePartMeshes;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Bridge", meta = (ClampMin = "2"))
	int32 MinPartCount = 2;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Bridge", meta = (ClampMin = "10.0"))
	float PartSpacing = 150.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Bridge")
	FVector PartScale = FVector(1.0f, 1.0f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Bridge")
	int32 RandomSeed = 1337;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Bridge")
	bool bAnchorBridgeEnds = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Bridge|Physics", meta = (ClampMin = "1.0"))
	float PartMass = 35.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Bridge|Physics", meta = (ClampMin = "0.0"))
	float PartLinearDamping = 0.6f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Bridge|Physics", meta = (ClampMin = "0.0"))
	float PartAngularDamping = 3.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Bridge|Physics", meta = (ClampMin = "0.0"))
	float Swing1LimitDegrees = 8.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Bridge|Physics", meta = (ClampMin = "0.0"))
	float Swing2LimitDegrees = 8.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Bridge|Physics", meta = (ClampMin = "0.0"))
	float TwistLimitDegrees = 4.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Bridge|Physics", meta = (ClampMin = "0.0"))
	float AngularStiffness = 900000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Bridge|Physics", meta = (ClampMin = "0.0"))
	float AngularDamping = 75000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Bridge|Physics", meta = (ClampMin = "0.0"))
	float ProjectionLinearTolerance = 4.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Bridge|Physics", meta = (ClampMin = "0.0"))
	float ProjectionAngularTolerance = 4.0f;

private:
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> GeneratedBridgeParts;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UPhysicsConstraintComponent>> GeneratedConstraints;
};
