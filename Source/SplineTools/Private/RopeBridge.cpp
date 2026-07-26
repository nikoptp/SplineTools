#include "RopeBridge.h"

#include "Components/SplineComponent.h"
#include "Components/StaticMeshComponent.h"
#include "PhysicsEngine/PhysicsConstraintComponent.h"

ARopeBridge::ARopeBridge()
{
	ToolSpline->SetClosedLoop(false);
	BridgeSpline = ToolSpline;
}

void ARopeBridge::RebuildBridge()
{
	RebuildSplineTool();
}

void ARopeBridge::ResetGeneratedContent()
{
	for (int32 ConstraintIndex = GeneratedConstraints.Num() - 1; ConstraintIndex >= 0; --ConstraintIndex)
	{
		if (!GeneratedConstraints[ConstraintIndex])
		{
			continue;
		}

		RemoveInstanceComponent(GeneratedConstraints[ConstraintIndex]);
		GeneratedConstraints[ConstraintIndex]->DestroyComponent();
	}

	GeneratedConstraints.Empty();

	for (int32 PartIndex = GeneratedBridgeParts.Num() - 1; PartIndex >= 0; --PartIndex)
	{
		if (!GeneratedBridgeParts[PartIndex])
		{
			continue;
		}

		RemoveInstanceComponent(GeneratedBridgeParts[PartIndex]);
		GeneratedBridgeParts[PartIndex]->DestroyComponent();
	}

	GeneratedBridgeParts.Empty();

	Super::ResetGeneratedContent();
}

void ARopeBridge::GenerateSplineContent()
{
	if (BridgePartMeshes.IsEmpty())
	{
		return;
	}

	const float SplineLength = BridgeSpline->GetSplineLength();
	if (SplineLength <= KINDA_SMALL_NUMBER)
	{
		return;
	}

	int32 PartCount = FMath::FloorToInt(SplineLength / FMath::Max(PartSpacing, 1.0f)) + 1;
	PartCount = FMath::Max(PartCount, MinPartCount);

	FRandomStream RandomStream(RandomSeed);
	const float DistanceStep = PartCount > 1 ? SplineLength / static_cast<float>(PartCount - 1) : 0.0f;

	for (int32 PartIndex = 0; PartIndex < PartCount; ++PartIndex)
	{
		CreateBridgePart(PartIndex, PartCount, DistanceStep * PartIndex, RandomStream);
	}

	for (int32 ConstraintIndex = 0; ConstraintIndex + 1 < GeneratedBridgeParts.Num(); ++ConstraintIndex)
	{
		CreateConstraint(ConstraintIndex);
	}
}

void ARopeBridge::CreateBridgePart(int32 PartIndex, int32 PartCount, float DistanceAlongSpline, FRandomStream& RandomStream)
{
	UStaticMesh* BridgePartMesh = PickBridgeMesh(RandomStream);
	if (!BridgePartMesh)
	{
		return;
	}

	const FTransform PartTransform = BridgeSpline->GetTransformAtDistanceAlongSpline(
		DistanceAlongSpline,
		ESplineCoordinateSpace::World,
		true
	);

	UStaticMeshComponent* BridgePart = NewObject<UStaticMeshComponent>(this, *FString::Printf(TEXT("BridgePart_%d"), PartIndex));
	AddInstanceComponent(BridgePart);
	BridgePart->SetMobility(EComponentMobility::Movable);
	BridgePart->SetStaticMesh(BridgePartMesh);
	BridgePart->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	BridgePart->SetCollisionObjectType(ECC_WorldStatic);
	BridgePart->SetCollisionProfileName(TEXT("BlockAll"));
	BridgePart->SetSimulatePhysics(!IsAnchoredPart(PartIndex, PartCount));
	BridgePart->SetEnableGravity(true);
	BridgePart->SetMassOverrideInKg(NAME_None, PartMass, true);
	BridgePart->SetLinearDamping(PartLinearDamping);
	BridgePart->SetAngularDamping(PartAngularDamping);
	BridgePart->SetupAttachment(SceneRoot);
	BridgePart->RegisterComponent();
	BridgePart->SetWorldTransform(PartTransform);
	BridgePart->SetWorldScale3D(PartScale);

	GeneratedBridgeParts.Add(BridgePart);
}

void ARopeBridge::CreateConstraint(int32 ConstraintIndex)
{
	UStaticMeshComponent* FirstPart = GeneratedBridgeParts.IsValidIndex(ConstraintIndex)
		? GeneratedBridgeParts[ConstraintIndex]
		: nullptr;
	UStaticMeshComponent* SecondPart = GeneratedBridgeParts.IsValidIndex(ConstraintIndex + 1)
		? GeneratedBridgeParts[ConstraintIndex + 1]
		: nullptr;

	if (!FirstPart || !SecondPart)
	{
		return;
	}

	UPhysicsConstraintComponent* Constraint = NewObject<UPhysicsConstraintComponent>(this, *FString::Printf(TEXT("BridgeConstraint_%d"), ConstraintIndex));
	AddInstanceComponent(Constraint);
	Constraint->SetMobility(EComponentMobility::Movable);
	Constraint->SetupAttachment(SceneRoot);
	Constraint->RegisterComponent();
	const FVector FirstLocation = FirstPart->GetComponentLocation();
	const FVector SecondLocation = SecondPart->GetComponentLocation();
	Constraint->SetWorldLocation((FirstLocation + SecondLocation) * 0.5f);
	Constraint->SetWorldRotation((SecondLocation - FirstLocation).Rotation());
	Constraint->SetDisableCollision(true);
	Constraint->SetLinearXLimit(ELinearConstraintMotion::LCM_Locked, 0.0f);
	Constraint->SetLinearYLimit(ELinearConstraintMotion::LCM_Locked, 0.0f);
	Constraint->SetLinearZLimit(ELinearConstraintMotion::LCM_Locked, 0.0f);
	Constraint->SetAngularSwing1Limit(EAngularConstraintMotion::ACM_Limited, Swing1LimitDegrees);
	Constraint->SetAngularSwing2Limit(EAngularConstraintMotion::ACM_Limited, Swing2LimitDegrees);
	Constraint->SetAngularTwistLimit(EAngularConstraintMotion::ACM_Limited, TwistLimitDegrees);
	Constraint->ConstraintInstance.ProfileInstance.ConeLimit.bSoftConstraint = true;
	Constraint->ConstraintInstance.ProfileInstance.ConeLimit.Stiffness = AngularStiffness;
	Constraint->ConstraintInstance.ProfileInstance.ConeLimit.Damping = AngularDamping;
	Constraint->ConstraintInstance.ProfileInstance.TwistLimit.bSoftConstraint = true;
	Constraint->ConstraintInstance.ProfileInstance.TwistLimit.Stiffness = AngularStiffness;
	Constraint->ConstraintInstance.ProfileInstance.TwistLimit.Damping = AngularDamping;
	Constraint->ConstraintInstance.ProfileInstance.bEnableProjection = true;
	Constraint->ConstraintInstance.ProfileInstance.ProjectionLinearTolerance = ProjectionLinearTolerance;
	Constraint->ConstraintInstance.ProfileInstance.ProjectionAngularTolerance = ProjectionAngularTolerance;
	Constraint->SetConstrainedComponents(FirstPart, NAME_None, SecondPart, NAME_None);

	GeneratedConstraints.Add(Constraint);
}

UStaticMesh* ARopeBridge::PickBridgeMesh(FRandomStream& RandomStream) const
{
	if (BridgePartMeshes.IsEmpty())
	{
		return nullptr;
	}

	TArray<UStaticMesh*> ValidMeshes;
	ValidMeshes.Reserve(BridgePartMeshes.Num());

	for (int32 MeshIndex = 0; MeshIndex < BridgePartMeshes.Num(); ++MeshIndex)
	{
		if (BridgePartMeshes[MeshIndex])
		{
			ValidMeshes.Add(BridgePartMeshes[MeshIndex]);
		}
	}

	if (ValidMeshes.IsEmpty())
	{
		return nullptr;
	}

	const int32 MeshIndex = RandomStream.RandRange(0, ValidMeshes.Num() - 1);
	return ValidMeshes[MeshIndex];
}

bool ARopeBridge::IsAnchoredPart(int32 PartIndex, int32 PartCount) const
{
	if (!bAnchorBridgeEnds)
	{
		return false;
	}

	return PartIndex == 0 || PartIndex == PartCount - 1;
}
