#include "CastleWallActor.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/SplineComponent.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"

ACastleWallActor::ACastleWallActor()
{
	ToolSpline->SetClosedLoop(false);
}

void ACastleWallActor::UpdateSplineSettings()
{
	ToolSpline->SetClosedLoop(bClosedLoop);
}

void ACastleWallActor::GenerateSplineContent()
{
	const float SplineLength = ToolSpline->GetSplineLength();
	if (SplineLength <= KINDA_SMALL_NUMBER)
	{
		return;
	}

	FRandomStream RandomStream(RandomSeed);
	TArray<float> TowerDistances;
	CollectTowerDistances(TowerDistances);

	TMap<TObjectPtr<UStaticMesh>, TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> WallComponents;
	TMap<TObjectPtr<UStaticMesh>, TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> TowerComponents;
	UStaticMesh* PreviousWallMesh = nullptr;

	if (!WallMeshes.IsEmpty())
	{
		int32 SegmentCount = FMath::FloorToInt(SplineLength / FMath::Max(WallSegmentSpacing, 1.0f));
		SegmentCount += bClosedLoop ? 0 : 1;
		SegmentCount = FMath::Max(SegmentCount, MinWallSegmentCount);

		const float DistanceStep = bClosedLoop
			? SplineLength / static_cast<float>(SegmentCount)
			: (SegmentCount > 1 ? SplineLength / static_cast<float>(SegmentCount - 1) : 0.0f);

		for (int32 SegmentIndex = 0; SegmentIndex < SegmentCount; ++SegmentIndex)
		{
			float DistanceAlongSpline = DistanceStep * SegmentIndex;
			if (!bClosedLoop && SegmentIndex == SegmentCount - 1)
			{
				DistanceAlongSpline = SplineLength;
			}

			if (HasTowerNearDistance(DistanceAlongSpline, TowerDistances, TowerExclusionRadius))
			{
				continue;
			}

			UStaticMesh* WallMesh = PickWallMesh(RandomStream, PreviousWallMesh);
			if (!WallMesh)
			{
				continue;
			}

			PreviousWallMesh = WallMesh;

			UHierarchicalInstancedStaticMeshComponent* WallComponent = WallComponents.FindRef(WallMesh);
			if (!WallComponent)
			{
				WallComponent = CreateGeneratedHISM(FString::Printf(TEXT("WallMesh_%d"), WallComponents.Num()), WallMesh);
				WallComponents.Add(WallMesh, WallComponent);
			}

			WallComponent->AddInstance(BuildCastlePieceTransform(DistanceAlongSpline, WallScale, WallOffset), true);
		}
	}

	if (TowerMeshes.IsEmpty())
	{
		return;
	}

	for (int32 TowerIndex = 0; TowerIndex < TowerDistances.Num(); ++TowerIndex)
	{
		UStaticMesh* TowerMesh = PickMesh(TowerMeshes, RandomStream);
		if (!TowerMesh)
		{
			continue;
		}

		UHierarchicalInstancedStaticMeshComponent* TowerComponent = TowerComponents.FindRef(TowerMesh);
		if (!TowerComponent)
		{
			TowerComponent = CreateGeneratedHISM(FString::Printf(TEXT("TowerMesh_%d"), TowerComponents.Num()), TowerMesh);
			TowerComponents.Add(TowerMesh, TowerComponent);
		}

		TowerComponent->AddInstance(BuildCastlePieceTransform(TowerDistances[TowerIndex], TowerScale, TowerOffset), true);
	}
}

FTransform ACastleWallActor::BuildCastlePieceTransform(float DistanceAlongSpline, const FVector& InstanceScale, const FVector& LocalOffset) const
{
	FTransform InstanceTransform = BuildSplineInstanceTransform(DistanceAlongSpline, InstanceScale);
	if (!bSnapToTerrain)
	{
		InstanceTransform.AddToTranslation(InstanceTransform.GetRotation().RotateVector(LocalOffset));
		return InstanceTransform;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return InstanceTransform;
	}

	const FVector OriginalLocation = InstanceTransform.GetLocation();
	const FVector TraceStart = OriginalLocation + FVector(0.0f, 0.0f, TerrainTraceHeightAbove);
	const FVector TraceEnd = OriginalLocation - FVector(0.0f, 0.0f, TerrainTraceHeightBelow);

	FHitResult HitResult;
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(CastleWallTerrainSnap), false, this);
	if (World->LineTraceSingleByChannel(HitResult, TraceStart, TraceEnd, ECC_Visibility, QueryParams))
	{
		InstanceTransform.SetLocation(HitResult.Location);
	}

	InstanceTransform.AddToTranslation(InstanceTransform.GetRotation().RotateVector(LocalOffset));
	return InstanceTransform;
}

void ACastleWallActor::CollectTowerDistances(TArray<float>& OutTowerDistances) const
{
	const int32 PointCount = ToolSpline->GetNumberOfSplinePoints();
	if (PointCount >= 3)
	{
		for (int32 PointIndex = 0; PointIndex < PointCount; ++PointIndex)
		{
			if (!bClosedLoop && (PointIndex == 0 || PointIndex == PointCount - 1))
			{
				continue;
			}

			if (!ShouldPlaceTowerAtPoint(PointIndex))
			{
				continue;
			}

			OutTowerDistances.Add(ToolSpline->GetDistanceAlongSplineAtSplinePoint(PointIndex));
		}
	}

	AddEvenlyDistributedTowerDistances(OutTowerDistances);
	OutTowerDistances.Sort();
}

void ACastleWallActor::AddEvenlyDistributedTowerDistances(TArray<float>& OutTowerDistances) const
{
	if (TowerAmount <= 0)
	{
		return;
	}

	const float SplineLength = ToolSpline->GetSplineLength();
	if (SplineLength <= KINDA_SMALL_NUMBER)
	{
		return;
	}

	const int32 RequestedTowerAmount = FMath::Max(TowerAmount, 0);
	const float DistanceStep = bClosedLoop
		? SplineLength / static_cast<float>(RequestedTowerAmount)
		: SplineLength / static_cast<float>(RequestedTowerAmount + 1);

	for (int32 TowerIndex = 0; TowerIndex < RequestedTowerAmount; ++TowerIndex)
	{
		const float DistanceAlongSpline = bClosedLoop
			? DistanceStep * TowerIndex
			: DistanceStep * (TowerIndex + 1);
		if (HasTowerNearDistance(DistanceAlongSpline, OutTowerDistances, TowerExclusionRadius))
		{
			continue;
		}

		OutTowerDistances.Add(DistanceAlongSpline);
	}
}

bool ACastleWallActor::ShouldPlaceTowerAtPoint(int32 PointIndex) const
{
	const int32 PointCount = ToolSpline->GetNumberOfSplinePoints();
	if (PointCount < 3)
	{
		return false;
	}

	int32 PreviousIndex = PointIndex - 1;
	int32 NextIndex = PointIndex + 1;

	if (PreviousIndex < 0)
	{
		PreviousIndex = PointCount - 1;
	}

	if (NextIndex >= PointCount)
	{
		NextIndex = 0;
	}

	const FVector PreviousLocation = ToolSpline->GetLocationAtSplinePoint(PreviousIndex, ESplineCoordinateSpace::World);
	const FVector CurrentLocation = ToolSpline->GetLocationAtSplinePoint(PointIndex, ESplineCoordinateSpace::World);
	const FVector NextLocation = ToolSpline->GetLocationAtSplinePoint(NextIndex, ESplineCoordinateSpace::World);

	const FVector IncomingDirection = (CurrentLocation - PreviousLocation).GetSafeNormal();
	const FVector OutgoingDirection = (NextLocation - CurrentLocation).GetSafeNormal();
	const float TurnDot = FMath::Clamp(FVector::DotProduct(IncomingDirection, OutgoingDirection), -1.0f, 1.0f);
	const float TurnAngleDegrees = FMath::RadiansToDegrees(FMath::Acos(TurnDot));
	return TurnAngleDegrees >= CornerTurnAngleThresholdDegrees;
}

bool ACastleWallActor::HasTowerNearDistance(float DistanceAlongSpline, const TArray<float>& TowerDistances, float Radius) const
{
	for (int32 TowerIndex = 0; TowerIndex < TowerDistances.Num(); ++TowerIndex)
	{
		const float DistanceToTower = FMath::Abs(TowerDistances[TowerIndex] - DistanceAlongSpline);
		const float WrappedDistanceToTower = ToolSpline->IsClosedLoop()
			? FMath::Min(DistanceToTower, ToolSpline->GetSplineLength() - DistanceToTower)
			: DistanceToTower;

		if (WrappedDistanceToTower <= Radius)
		{
			return true;
		}
	}

	return false;
}

UStaticMesh* ACastleWallActor::PickWallMesh(FRandomStream& RandomStream, UStaticMesh* PreviousWallMesh) const
{
	TArray<UStaticMesh*> ValidMeshes;
	ValidMeshes.Reserve(WallMeshes.Num());

	for (int32 MeshIndex = 0; MeshIndex < WallMeshes.Num(); ++MeshIndex)
	{
		if (WallMeshes[MeshIndex])
		{
			ValidMeshes.Add(WallMeshes[MeshIndex]);
		}
	}

	if (ValidMeshes.IsEmpty())
	{
		return nullptr;
	}

	if (ValidMeshes.Num() == 1 || !PreviousWallMesh)
	{
		const int32 MeshIndex = RandomStream.RandRange(0, ValidMeshes.Num() - 1);
		return ValidMeshes[MeshIndex];
	}

	TArray<UStaticMesh*> CandidateMeshes;
	CandidateMeshes.Reserve(ValidMeshes.Num() - 1);

	for (int32 MeshIndex = 0; MeshIndex < ValidMeshes.Num(); ++MeshIndex)
	{
		if (ValidMeshes[MeshIndex] != PreviousWallMesh)
		{
			CandidateMeshes.Add(ValidMeshes[MeshIndex]);
		}
	}

	if (CandidateMeshes.IsEmpty())
	{
		return PreviousWallMesh;
	}

	const int32 MeshIndex = RandomStream.RandRange(0, CandidateMeshes.Num() - 1);
	return CandidateMeshes[MeshIndex];
}

UStaticMesh* ACastleWallActor::PickMesh(const TArray<TObjectPtr<UStaticMesh>>& MeshOptions, FRandomStream& RandomStream) const
{
	TArray<UStaticMesh*> ValidMeshes;
	ValidMeshes.Reserve(MeshOptions.Num());

	for (int32 MeshIndex = 0; MeshIndex < MeshOptions.Num(); ++MeshIndex)
	{
		if (MeshOptions[MeshIndex])
		{
			ValidMeshes.Add(MeshOptions[MeshIndex]);
		}
	}

	if (ValidMeshes.IsEmpty())
	{
		return nullptr;
	}

	const int32 MeshIndex = RandomStream.RandRange(0, ValidMeshes.Num() - 1);
	return ValidMeshes[MeshIndex];
}
