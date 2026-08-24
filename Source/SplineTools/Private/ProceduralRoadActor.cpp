#include "ProceduralRoadActor.h"

#include "Components/DecalComponent.h"
#include "Components/SplineComponent.h"
#include "Engine/World.h"
#include "KismetProceduralMeshLibrary.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"
#include "ProceduralRoadJunctionActor.h"

struct FSplineRoadCrossSection
{
	float DistanceAlongSpline = 0.0f;
	TArray<FVector> RoadPoints;
	FVector LeftFlapPoint = FVector::ZeroVector;
	FVector RightFlapPoint = FVector::ZeroVector;
};

namespace
{
	void AddQuad(
		TArray<int32>& Triangles,
		int32 FirstCurrent,
		int32 FirstNext,
		int32 SecondCurrent,
		int32 SecondNext)
	{
		Triangles.Add(FirstCurrent);
		Triangles.Add(FirstNext);
		Triangles.Add(SecondCurrent);
		Triangles.Add(SecondCurrent);
		Triangles.Add(FirstNext);
		Triangles.Add(SecondNext);
	}

	FVector SampleCrossSection(
		const FSplineRoadCrossSection& CrossSection,
		float RoadWidth,
		float LateralOffset)
	{
		if (CrossSection.RoadPoints.Num() == 1)
		{
			return CrossSection.RoadPoints[0];
		}

		const float WidthAlpha = FMath::Clamp(
			(LateralOffset + RoadWidth * 0.5f) / FMath::Max(RoadWidth, 1.0f),
			0.0f,
			1.0f);
		const float PointPosition =
			WidthAlpha * static_cast<float>(CrossSection.RoadPoints.Num() - 1);
		const int32 FirstPointIndex = FMath::Min(
			FMath::FloorToInt(PointPosition),
			CrossSection.RoadPoints.Num() - 2);
		return FMath::Lerp(
			CrossSection.RoadPoints[FirstPointIndex],
			CrossSection.RoadPoints[FirstPointIndex + 1],
			PointPosition - static_cast<float>(FirstPointIndex));
	}

	FVector SampleCrossSectionAtDistance(
		const TArray<FSplineRoadCrossSection>& CrossSections,
		float RoadWidth,
		float DistanceAlongSpline,
		float LateralOffset)
	{
		if (DistanceAlongSpline <= CrossSections[0].DistanceAlongSpline)
		{
			return SampleCrossSection(CrossSections[0], RoadWidth, LateralOffset);
		}

		for (int32 SectionIndex = 0; SectionIndex + 1 < CrossSections.Num(); ++SectionIndex)
		{
			if (DistanceAlongSpline > CrossSections[SectionIndex + 1].DistanceAlongSpline)
			{
				continue;
			}

			const float DistanceRange =
				CrossSections[SectionIndex + 1].DistanceAlongSpline
				- CrossSections[SectionIndex].DistanceAlongSpline;
			const float Alpha = DistanceRange > KINDA_SMALL_NUMBER
				? (DistanceAlongSpline - CrossSections[SectionIndex].DistanceAlongSpline)
					/ DistanceRange
				: 0.0f;
			return FMath::Lerp(
				SampleCrossSection(
					CrossSections[SectionIndex],
					RoadWidth,
					LateralOffset),
				SampleCrossSection(
					CrossSections[SectionIndex + 1],
					RoadWidth,
					LateralOffset),
				Alpha);
		}

		return SampleCrossSection(CrossSections.Last(), RoadWidth, LateralOffset);
	}
}

AProceduralRoadActor::AProceduralRoadActor()
{
	ToolSpline->SetClosedLoop(false);

	RoadMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("RoadMesh"));
	RoadMesh->SetupAttachment(ToolSpline);
	RoadMesh->SetMobility(EComponentMobility::Movable);
	RoadMesh->bUseAsyncCooking = true;
	RoadMesh->bUseComplexAsSimpleCollision = false;
	RoadMesh->SetCollisionProfileName(TEXT("BlockAll"));
	GeneratedRoadChunks.Add(RoadMesh);
}

void AProceduralRoadActor::PostLoad()
{
	Super::PostLoad();
	RestoreCachedRoadComponents();
}

void AProceduralRoadActor::PostDuplicate(EDuplicateMode::Type DuplicateMode)
{
	Super::PostDuplicate(DuplicateMode);
	RestoreCachedRoadComponents();
}

void AProceduralRoadActor::PostInitializeComponents()
{
	Super::PostInitializeComponents();
	RestoreCachedRoadComponents();
}

#if WITH_EDITOR
void AProceduralRoadActor::PostEditUndo()
{
	RestoreCachedRoadComponents();
	Super::PostEditUndo();
}
#endif

void AProceduralRoadActor::RebuildSplineTool()
{
	UpdateSplineSettings();
	RestoreCachedRoadComponents();
	if (!IsSplineUsable())
	{
		ResetGeneratedContent();
	}
	else
	{
		UpdateRoadChunks();
		ResetGeneratedDecals();
		GenerateDecals();
	}

#if WITH_EDITOR
	if (!bEditorRebuildDeferred)
	{
		AProceduralRoadJunctionActor::NotifyRoadEdited(this);
	}
#endif
}

void AProceduralRoadActor::RebuildRoad()
{
#if WITH_EDITOR
	const bool bIsEditorWorld = !GetWorld() || !GetWorld()->IsGameWorld();
	if (bIsEditorWorld)
	{
		Modify();
	}
#endif

	RoadChunkSourceHashes.Reset();
	RebuildSplineTool();

#if WITH_EDITOR
	if (bIsEditorWorld)
	{
		MarkPackageDirty();
	}
#endif
}

bool AProceduralRoadActor::HasCachedRoadData() const
{
	for (const UProceduralMeshComponent* RoadChunk : GeneratedRoadChunks)
	{
		if (IsValid(RoadChunk) && RoadChunk->GetNumSections() > 0)
		{
			return true;
		}
	}
	return false;
}

UMaterialInterface* AProceduralRoadActor::GetRoadMaterial() const
{
	return RoadMaterial;
}

UMaterialInterface* AProceduralRoadActor::GetRoadSideFlapMaterial() const
{
	return SideFlapMaterial ? SideFlapMaterial.Get() : RoadMaterial.Get();
}

float AProceduralRoadActor::GetRoadWidth() const
{
	return RoadWidth;
}

const FProceduralRoadLandscapePaintSettings& AProceduralRoadActor::GetLandscapePaintSettings() const
{
	return LandscapePaintSettings;
}

bool AProceduralRoadActor::SetJunctionTrim(
	ERoadSplineEndpoint Endpoint,
	float TrimDistance,
	AActor* JunctionOwner)
{
	if (!JunctionOwner)
	{
		return false;
	}

	TWeakObjectPtr<AActor>& CurrentOwner = Endpoint == ERoadSplineEndpoint::Start
		? StartJunctionOwner
		: EndJunctionOwner;
	float& CurrentTrimDistance = Endpoint == ERoadSplineEndpoint::Start
		? StartJunctionTrimDistance
		: EndJunctionTrimDistance;
	if (CurrentOwner.IsValid() && CurrentOwner.Get() != JunctionOwner)
	{
		return false;
	}

	const float NewTrimDistance = FMath::Max(TrimDistance, 0.0f);
	if (CurrentOwner.Get() == JunctionOwner
		&& FMath::IsNearlyEqual(CurrentTrimDistance, NewTrimDistance))
	{
		return true;
	}

	CurrentOwner = JunctionOwner;
	CurrentTrimDistance = NewTrimDistance;
#if WITH_EDITOR
	if (bEditorRebuildDeferred)
	{
		return true;
	}
#endif
	if (!GetWorld() || !GetWorld()->IsGameWorld() || ShouldRebuildInGameWorld())
	{
		RebuildSplineTool();
	}
	return true;
}

void AProceduralRoadActor::ClearJunctionTrim(
	ERoadSplineEndpoint Endpoint,
	AActor* JunctionOwner)
{
	TWeakObjectPtr<AActor>& CurrentOwner = Endpoint == ERoadSplineEndpoint::Start
		? StartJunctionOwner
		: EndJunctionOwner;
	float& CurrentTrimDistance = Endpoint == ERoadSplineEndpoint::Start
		? StartJunctionTrimDistance
		: EndJunctionTrimDistance;
	if (CurrentOwner.Get() != JunctionOwner)
	{
		return;
	}

	CurrentOwner.Reset();
	CurrentTrimDistance = 0.0f;
#if WITH_EDITOR
	if (bEditorRebuildDeferred)
	{
		return;
	}
#endif
	if (!GetWorld() || !GetWorld()->IsGameWorld() || ShouldRebuildInGameWorld())
	{
		RebuildSplineTool();
	}
}

bool AProceduralRoadActor::GetJunctionEdge(
	ERoadSplineEndpoint Endpoint,
	float TrimDistance,
	FVector& OutCenter,
	FVector& OutLeft,
	FVector& OutRight,
	FVector& OutDirection) const
{
	FProceduralRoadJunctionEdgeGeometry Geometry;
	if (!GetJunctionEdgeGeometry(Endpoint, TrimDistance, Geometry))
	{
		return false;
	}

	OutCenter = FVector::ZeroVector;
	for (const FVector& SurfacePoint : Geometry.SurfacePoints)
	{
		OutCenter += SurfacePoint;
	}
	OutCenter /= Geometry.SurfacePoints.Num();
	OutLeft = Geometry.SurfacePoints[0];
	OutRight = Geometry.SurfacePoints.Last();
	OutDirection = Geometry.Direction;
	return true;
}

bool AProceduralRoadActor::GetJunctionEdgeSamples(
	ERoadSplineEndpoint Endpoint,
	float TrimDistance,
	TArray<FVector>& OutEdgePoints,
	FVector& OutDirection) const
{
	FProceduralRoadJunctionEdgeGeometry Geometry;
	if (!GetJunctionEdgeGeometry(Endpoint, TrimDistance, Geometry))
	{
		OutEdgePoints.Reset();
		return false;
	}

	OutEdgePoints = MoveTemp(Geometry.SurfacePoints);
	OutDirection = Geometry.Direction;
	return true;
}

bool AProceduralRoadActor::GetJunctionEdgeGeometry(
	ERoadSplineEndpoint Endpoint,
	float TrimDistance,
	FProceduralRoadJunctionEdgeGeometry& OutGeometry) const
{
	OutGeometry = FProceduralRoadJunctionEdgeGeometry();
	if (!IsSplineUsable())
	{
		return false;
	}

	const float SplineLength = ToolSpline->GetSplineLength();
	const float Distance = Endpoint == ERoadSplineEndpoint::Start
		? FMath::Clamp(TrimDistance, 0.0f, SplineLength)
		: FMath::Clamp(SplineLength - TrimDistance, 0.0f, SplineLength);
	OutGeometry.Direction = ToolSpline->GetTangentAtDistanceAlongSpline(
		Distance,
		ESplineCoordinateSpace::World).GetSafeNormal();
	if (Endpoint == ERoadSplineEndpoint::Start)
	{
		OutGeometry.Direction *= -1.0f;
	}

	const int32 SubdivisionCount = FMath::Max(WidthSubdivisions, 1);
	const int32 RoadPointCount = SubdivisionCount + 1;
	UProceduralMeshComponent* EndpointChunk = nullptr;
	if (Endpoint == ERoadSplineEndpoint::Start)
	{
		for (UProceduralMeshComponent* RoadChunk : GeneratedRoadChunks)
		{
			if (IsValid(RoadChunk) && RoadChunk->GetProcMeshSection(0))
			{
				EndpointChunk = RoadChunk;
				break;
			}
		}
	}
	else
	{
		for (int32 ChunkIndex = GeneratedRoadChunks.Num() - 1;
			ChunkIndex >= 0;
			--ChunkIndex)
		{
			UProceduralMeshComponent* RoadChunk = GeneratedRoadChunks[ChunkIndex];
			if (IsValid(RoadChunk) && RoadChunk->GetProcMeshSection(0))
			{
				EndpointChunk = RoadChunk;
				break;
			}
		}
	}

	if (EndpointChunk)
	{
		const FProcMeshSection* SurfaceSection = EndpointChunk->GetProcMeshSection(0);
		if (SurfaceSection
			&& SurfaceSection->ProcVertexBuffer.Num() >= RoadPointCount
			&& SurfaceSection->ProcVertexBuffer.Num() % RoadPointCount == 0)
		{
			const int32 CrossSectionCount =
				SurfaceSection->ProcVertexBuffer.Num() / RoadPointCount;
			const int32 SurfaceStart = Endpoint == ERoadSplineEndpoint::Start
				? 0
				: (CrossSectionCount - 1) * RoadPointCount;
			OutGeometry.SurfacePoints.Reserve(RoadPointCount);
			for (int32 WidthIndex = 0; WidthIndex < RoadPointCount; ++WidthIndex)
			{
				OutGeometry.SurfacePoints.Add(
					EndpointChunk->GetComponentTransform().TransformPosition(
						SurfaceSection->ProcVertexBuffer[SurfaceStart + WidthIndex].Position));
			}

			const FProcMeshSection* FlapSection = EndpointChunk->GetProcMeshSection(1);
			const int32 RequiredFlapVertexCount = CrossSectionCount * 4;
			if (SideFlapWidth > KINDA_SMALL_NUMBER
				&& FlapSection
				&& FlapSection->ProcVertexBuffer.Num() >= RequiredFlapVertexCount)
			{
				const int32 FlapStart = Endpoint == ERoadSplineEndpoint::Start
					? 0
					: (CrossSectionCount - 1) * 4;
				OutGeometry.LeftFlapPoint =
					EndpointChunk->GetComponentTransform().TransformPosition(
						FlapSection->ProcVertexBuffer[FlapStart].Position);
				OutGeometry.RightFlapPoint =
					EndpointChunk->GetComponentTransform().TransformPosition(
						FlapSection->ProcVertexBuffer[FlapStart + 3].Position);
				OutGeometry.bHasSideFlaps = true;
			}
			OutGeometry.bUsesCachedMesh = true;
			return true;
		}
	}

	OutGeometry.SurfacePoints.Reserve(RoadPointCount);
	for (int32 WidthIndex = 0; WidthIndex <= SubdivisionCount; ++WidthIndex)
	{
		const float LateralAlpha = static_cast<float>(WidthIndex) / SubdivisionCount;
		OutGeometry.SurfacePoints.Add(SampleRoadPosition(
			Distance,
			FMath::Lerp(-RoadWidth * 0.5f, RoadWidth * 0.5f, LateralAlpha)));
	}
	if (SideFlapWidth > KINDA_SMALL_NUMBER)
	{
		OutGeometry.LeftFlapPoint = SampleRoadPosition(
			Distance,
			-RoadWidth * 0.5f - SideFlapWidth)
			- FVector::UpVector * SideFlapEmbedDepth;
		OutGeometry.RightFlapPoint = SampleRoadPosition(
			Distance,
			RoadWidth * 0.5f + SideFlapWidth)
			- FVector::UpVector * SideFlapEmbedDepth;
		OutGeometry.bHasSideFlaps = true;
	}
	return true;
}

float AProceduralRoadActor::GetRoadSplineLength() const
{
	return IsSplineUsable() ? ToolSpline->GetSplineLength() : 0.0f;
}

bool AProceduralRoadActor::GetJunctionTrimDistance(
	ERoadSplineEndpoint Endpoint,
	float& OutTrimDistance) const
{
	const TWeakObjectPtr<AActor>& JunctionOwner =
		Endpoint == ERoadSplineEndpoint::Start
			? StartJunctionOwner
			: EndJunctionOwner;
	OutTrimDistance = Endpoint == ERoadSplineEndpoint::Start
		? StartJunctionTrimDistance
		: EndJunctionTrimDistance;
	return JunctionOwner.IsValid();
}

bool AProceduralRoadActor::GetSplineEndpointLocation(
	ERoadSplineEndpoint Endpoint,
	FVector& OutLocation) const
{
	if (!IsSplineUsable())
	{
		return false;
	}

	const float Distance = Endpoint == ERoadSplineEndpoint::Start
		? 0.0f
		: ToolSpline->GetSplineLength();
	OutLocation = ToolSpline->GetLocationAtDistanceAlongSpline(
		Distance,
		ESplineCoordinateSpace::World);
	return true;
}

bool AProceduralRoadActor::IsJunctionEndpointAvailable(
	ERoadSplineEndpoint Endpoint,
	const AActor* JunctionOwner) const
{
	const TWeakObjectPtr<AActor>& CurrentOwner =
		Endpoint == ERoadSplineEndpoint::Start
			? StartJunctionOwner
			: EndJunctionOwner;
	return !CurrentOwner.IsValid() || CurrentOwner.Get() == JunctionOwner;
}

#if WITH_EDITOR
void AProceduralRoadActor::GetRoadSplinePoints(
	TArray<FProceduralRoadSplinePoint>& OutPoints) const
{
	OutPoints.Reset(ToolSpline->GetNumberOfSplinePoints());
	for (int32 PointIndex = 0;
		PointIndex < ToolSpline->GetNumberOfSplinePoints();
		++PointIndex)
	{
		FProceduralRoadSplinePoint& Point = OutPoints.AddDefaulted_GetRef();
		Point.WorldLocation = ToolSpline->GetLocationAtSplinePoint(
			PointIndex,
			ESplineCoordinateSpace::World);
		Point.WorldArriveTangent = ToolSpline->GetArriveTangentAtSplinePoint(
			PointIndex,
			ESplineCoordinateSpace::World);
		Point.WorldLeaveTangent = ToolSpline->GetLeaveTangentAtSplinePoint(
			PointIndex,
			ESplineCoordinateSpace::World);
		Point.Rotation = ToolSpline->GetRotationAtSplinePoint(
			PointIndex,
			ESplineCoordinateSpace::World);
		Point.Scale = ToolSpline->GetScaleAtSplinePoint(PointIndex);
		Point.Type = ToolSpline->GetSplinePointType(PointIndex);
	}
}

void AProceduralRoadActor::SetRoadSplinePoints(
	const TArray<FProceduralRoadSplinePoint>& Points,
	bool bInClosedLoop,
	bool bRebuild)
{
	Modify();
	ToolSpline->Modify();
	ToolSpline->ClearSplinePoints(false);
	for (int32 PointIndex = 0; PointIndex < Points.Num(); ++PointIndex)
	{
		FSplinePoint SplinePoint;
		SplinePoint.InputKey = static_cast<float>(PointIndex);
		SplinePoint.Position = ToolSpline->GetComponentTransform()
			.InverseTransformPosition(Points[PointIndex].WorldLocation);
		SplinePoint.ArriveTangent = ToolSpline->GetComponentTransform()
			.InverseTransformVectorNoScale(Points[PointIndex].WorldArriveTangent);
		SplinePoint.LeaveTangent = ToolSpline->GetComponentTransform()
			.InverseTransformVectorNoScale(Points[PointIndex].WorldLeaveTangent);
		SplinePoint.Rotation = Points[PointIndex].Rotation;
		SplinePoint.Scale = Points[PointIndex].Scale;
		SplinePoint.Type = Points[PointIndex].Type;
		ToolSpline->AddPoint(SplinePoint, false);
	}
	ToolSpline->SetClosedLoop(bInClosedLoop, false);
	ToolSpline->UpdateSpline();
	bClosedLoop = bInClosedLoop;
	if (bRebuild && !bEditorRebuildDeferred)
	{
		RebuildRoad();
	}
}

void AProceduralRoadActor::SampleRoadSplineSegment(
	int32 SegmentIndex,
	float SampleInterval,
	TArray<FVector>& OutWorldPoints) const
{
	OutWorldPoints.Reset();
	if (!ToolSpline || ToolSpline->GetNumberOfSplinePoints() < 2)
	{
		return;
	}

	const int32 PointCount = ToolSpline->GetNumberOfSplinePoints();
	const int32 SegmentCount = ToolSpline->IsClosedLoop() ? PointCount : PointCount - 1;
	if (SegmentIndex < 0 || SegmentIndex >= SegmentCount)
	{
		return;
	}

	const float StartDistance = ToolSpline->GetDistanceAlongSplineAtSplinePoint(SegmentIndex);
	const float EndDistance = SegmentIndex + 1 < PointCount
		? ToolSpline->GetDistanceAlongSplineAtSplinePoint(SegmentIndex + 1)
		: ToolSpline->GetSplineLength();
	const int32 SampleCount = FMath::Max(
		1,
		FMath::CeilToInt((EndDistance - StartDistance) / FMath::Max(1.0f, SampleInterval)));
	OutWorldPoints.Reserve(SampleCount + 1);
	for (int32 SampleIndex = 0; SampleIndex <= SampleCount; ++SampleIndex)
	{
		const float Distance = FMath::Lerp(
			StartDistance,
			EndDistance,
			static_cast<float>(SampleIndex) / SampleCount);
		OutWorldPoints.Add(ToolSpline->GetLocationAtDistanceAlongSpline(
			Distance,
			ESplineCoordinateSpace::World));
	}
}

bool AProceduralRoadActor::IsRoadSplineClosedLoop() const
{
	return ToolSpline && ToolSpline->IsClosedLoop();
}

void AProceduralRoadActor::SetManagedRoadIdentity(
	const FGuid& NetworkId,
	const FGuid& RunId)
{
	Modify();
	ManagedRoadNetworkId = NetworkId;
	ManagedRoadRunId = RunId;
}

bool AProceduralRoadActor::IsManagedByRoadNetwork(const FGuid& NetworkId) const
{
	return NetworkId.IsValid() && ManagedRoadNetworkId == NetworkId;
}

FGuid AProceduralRoadActor::GetManagedRoadRunId() const
{
	return ManagedRoadRunId;
}

void AProceduralRoadActor::SetEditorRebuildDeferred(bool bDeferred)
{
	bEditorRebuildDeferred = bDeferred;
}
#endif

void AProceduralRoadActor::UpdateSplineSettings()
{
	ToolSpline->SetClosedLoop(bClosedLoop);
}

void AProceduralRoadActor::ResetGeneratedContent()
{
	if (!GeneratedRoadChunks.Contains(RoadMesh))
	{
		GeneratedRoadChunks.Insert(RoadMesh, 0);
	}

	for (int32 ChunkIndex = GeneratedRoadChunks.Num() - 1; ChunkIndex >= 0; --ChunkIndex)
	{
		UProceduralMeshComponent* RoadChunk = GeneratedRoadChunks[ChunkIndex];
		if (!RoadChunk)
		{
			continue;
		}

		RoadChunk->ClearAllMeshSections();
		RoadChunk->ClearCollisionConvexMeshes();
		if (RoadChunk != RoadMesh)
		{
			RemoveInstanceComponent(RoadChunk);
			RoadChunk->DestroyComponent();
		}
	}
	GeneratedRoadChunks.Reset();
	GeneratedRoadChunks.Add(RoadMesh);
	RoadChunkSourceHashes.Reset();

	ResetGeneratedDecals();
	Super::ResetGeneratedContent();
}

void AProceduralRoadActor::RestoreCachedRoadComponents()
{
	TArray<UProceduralMeshComponent*> RoadComponents;
	GetComponents(RoadComponents);
	for (UProceduralMeshComponent* RoadComponent : RoadComponents)
	{
		if (RoadComponent
			&& (RoadComponent == RoadMesh
				|| RoadComponent->GetName().StartsWith(TEXT("RoadChunk_")))
			&& !GeneratedRoadChunks.Contains(RoadComponent))
		{
			GeneratedRoadChunks.Add(RoadComponent);
		}
	}

	GeneratedRoadChunks.RemoveAll(
		[](const TObjectPtr<UProceduralMeshComponent>& RoadComponent)
		{
			return !IsValid(RoadComponent);
		});
	GeneratedRoadChunks.StableSort(
		[this](
			const TObjectPtr<UProceduralMeshComponent>& Left,
			const TObjectPtr<UProceduralMeshComponent>& Right)
		{
			if (Left == Right)
			{
				return false;
			}
			if (Left == RoadMesh)
			{
				return true;
			}
			if (Right == RoadMesh)
			{
				return false;
			}

			int32 LeftIndex = MAX_int32;
			int32 RightIndex = MAX_int32;
			LexTryParseString(LeftIndex, *Left->GetName().RightChop(10));
			LexTryParseString(RightIndex, *Right->GetName().RightChop(10));
			return LeftIndex < RightIndex;
		});

	TArray<UDecalComponent*> DecalComponents;
	GetComponents(DecalComponents);
	for (UDecalComponent* DecalComponent : DecalComponents)
	{
		if (DecalComponent
			&& DecalComponent->GetName().StartsWith(TEXT("RoadDecal_"))
			&& !GeneratedDecals.Contains(DecalComponent))
		{
			GeneratedDecals.Add(DecalComponent);
		}
	}
	GeneratedDecals.RemoveAll(
		[](const TObjectPtr<UDecalComponent>& DecalComponent)
		{
			return !IsValid(DecalComponent);
		});
}

bool AProceduralRoadActor::ShouldRebuildInGameWorld() const
{
	// Terrain-conforming roads are authored against the complete editor landscape.
	// Rebuilding during PIE can run before World Partition landscape collision is
	// present and replace the saved road with spline-height fallback geometry.
	return false;
}

void AProceduralRoadActor::ResetGeneratedDecals()
{
	TArray<UDecalComponent*> DecalComponents;
	GetComponents(DecalComponents);
	for (UDecalComponent* DecalComponent : DecalComponents)
	{
		if (DecalComponent
			&& DecalComponent->GetName().StartsWith(TEXT("RoadDecal_"))
			&& !GeneratedDecals.Contains(DecalComponent))
		{
			GeneratedDecals.Add(DecalComponent);
		}
	}

	for (int32 DecalIndex = GeneratedDecals.Num() - 1; DecalIndex >= 0; --DecalIndex)
	{
		if (!GeneratedDecals[DecalIndex])
		{
			continue;
		}

		RemoveInstanceComponent(GeneratedDecals[DecalIndex]);
		GeneratedDecals[DecalIndex]->DestroyComponent();
	}
	GeneratedDecals.Empty();
}

void AProceduralRoadActor::UpdateRoadChunks()
{
	GeneratedRoadChunks.RemoveAll(
		[](const TObjectPtr<UProceduralMeshComponent>& RoadChunk)
		{
			return !RoadChunk;
		});
	if (!GeneratedRoadChunks.Contains(RoadMesh))
	{
		GeneratedRoadChunks.Insert(RoadMesh, 0);
	}

	const float RoadStartDistance = GetEffectiveStartDistance();
	const float RoadEndDistance = GetEffectiveEndDistance();
	const float RoadLength = RoadEndDistance - RoadStartDistance;
	if (RoadLength <= KINDA_SMALL_NUMBER)
	{
		ResetGeneratedContent();
		return;
	}

	const int32 ChunkCount = FMath::Max(
		FMath::CeilToInt(RoadLength / FMath::Max(ChunkLength, 100.0f)),
		1);
	EnsureRoadChunkCount(ChunkCount);
	RoadChunkSourceHashes.SetNum(ChunkCount);

	for (int32 ChunkIndex = 0; ChunkIndex < ChunkCount; ++ChunkIndex)
	{
		const float StartDistance = FMath::Min(
			RoadStartDistance
				+ ChunkIndex * FMath::Max(ChunkLength, 100.0f),
			RoadEndDistance);
		const float EndDistance = ChunkIndex == ChunkCount - 1
			? RoadEndDistance
			: FMath::Min(
				RoadStartDistance
					+ (ChunkIndex + 1) * FMath::Max(ChunkLength, 100.0f),
				RoadEndDistance);

		UProceduralMeshComponent* RoadChunk = GeneratedRoadChunks[ChunkIndex];
		ConfigureRoadChunk(RoadChunk);
		RoadChunk->SetMaterial(0, RoadMaterial);
		RoadChunk->SetMaterial(
			1,
			SideFlapMaterial ? SideFlapMaterial.Get() : RoadMaterial.Get());
		RoadChunk->SetMaterial(2, RoadLineMaterial);
		RoadChunk->SetMaterial(3, GetCenterLineMaterial());

		const uint32 ChunkSourceHash = CalculateChunkSourceHash(
			StartDistance,
			EndDistance);
		if (RoadChunkSourceHashes[ChunkIndex] == ChunkSourceHash
			&& RoadChunk->GetNumSections() > 0)
		{
			continue;
		}

		TArray<FSplineRoadCrossSection> CrossSections;
		BuildCrossSections(StartDistance, EndDistance, CrossSections);
		RebuildRoadChunk(RoadChunk, CrossSections);
		RoadChunkSourceHashes[ChunkIndex] = ChunkSourceHash;
	}
}

void AProceduralRoadActor::EnsureRoadChunkCount(int32 RequiredChunkCount)
{
	while (GeneratedRoadChunks.Num() > RequiredChunkCount)
	{
		UProceduralMeshComponent* RoadChunk = GeneratedRoadChunks.Pop();
		if (RoadChunk && RoadChunk != RoadMesh)
		{
			RemoveInstanceComponent(RoadChunk);
			RoadChunk->DestroyComponent();
		}
	}

	while (GeneratedRoadChunks.Num() < RequiredChunkCount)
	{
		GeneratedRoadChunks.Add(CreateRoadChunk(GeneratedRoadChunks.Num()));
	}
}

UProceduralMeshComponent* AProceduralRoadActor::CreateRoadChunk(int32 ChunkIndex)
{
	UProceduralMeshComponent* RoadChunk = NewObject<UProceduralMeshComponent>(
		this,
		*FString::Printf(TEXT("RoadChunk_%d"), ChunkIndex));
	AddInstanceComponent(RoadChunk);
	RoadChunk->SetupAttachment(ToolSpline);
	ConfigureRoadChunk(RoadChunk);
	RoadChunk->RegisterComponent();
	return RoadChunk;
}

void AProceduralRoadActor::ConfigureRoadChunk(
	UProceduralMeshComponent* RoadChunk) const
{
	if (!RoadChunk)
	{
		return;
	}

	RoadChunk->SetMobility(EComponentMobility::Movable);
	RoadChunk->bUseAsyncCooking = true;
	RoadChunk->bUseComplexAsSimpleCollision = false;
	RoadChunk->SetCollisionProfileName(TEXT("BlockAll"));
	RoadChunk->SetCollisionEnabled(
		bGenerateCollision
			? ECollisionEnabled::QueryAndPhysics
			: ECollisionEnabled::NoCollision);
}

void AProceduralRoadActor::RebuildRoadChunk(
	UProceduralMeshComponent* RoadChunk,
	const TArray<FSplineRoadCrossSection>& CrossSections)
{
	if (!RoadChunk || CrossSections.Num() < 2)
	{
		return;
	}

	RoadChunk->ClearAllMeshSections();
	RoadChunk->ClearCollisionConvexMeshes();
	BuildRoadSurface(RoadChunk, CrossSections);
	BuildSideFlaps(RoadChunk, CrossSections);
	BuildRoadLines(RoadChunk, CrossSections);
	BuildSimpleCollision(RoadChunk, CrossSections);
	RoadChunk->MarkRenderStateDirty();
}

uint32 AProceduralRoadActor::CalculateChunkSourceHash(
	float StartDistance,
	float EndDistance) const
{
	uint32 ChunkHash = GetTypeHash(RoadWidth);
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(SegmentLength));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(WidthSubdivisions));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(UVWorldSize));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(MarkingUVWorldLength));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(SideFlapWidth));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(SideFlapEmbedDepth));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(bGenerateEndFlaps));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(EndFlapLength));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(EndFlapEmbedDepth));
	ChunkHash = HashCombine(ChunkHash, PointerHash(RoadLineMaterial.Get()));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(bGenerateCenterLine));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(bGenerateSideLines));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(RoadLineWidth));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(SideLineInset));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(RoadLineSurfaceOffset));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(RoadLineUVWorldLength));
	ChunkHash = HashCombine(ChunkHash, PointerHash(CenterLineMaterial.Get()));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(CenterLineWidth));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(CenterLineSurfaceOffset));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(CenterLineUVWorldLength));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(bCenterLineHasGaps));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(CenterLineDashLength));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(CenterLineGapLength));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(bGenerateCollision));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(CollisionThickness));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(CollisionSegmentStride));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(bAlignToTerrain));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(TerrainTraceHeightAbove));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(TerrainTraceHeightBelow));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(TerrainTraceChannel.GetValue()));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(RoadSurfaceOffset));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(StartDistance));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(EndDistance));

	const float ChunkDistance = FMath::Max(EndDistance - StartDistance, 0.0f);
	const int32 SegmentCount = FMath::Max(
		FMath::CeilToInt(ChunkDistance / FMath::Max(SegmentLength, 10.0f)),
		1);
	const float DistanceStep = ChunkDistance / static_cast<float>(SegmentCount);
	for (int32 SegmentIndex = 0; SegmentIndex <= SegmentCount; ++SegmentIndex)
	{
		const float Distance = SegmentIndex == SegmentCount
			? EndDistance
			: StartDistance + DistanceStep * SegmentIndex;
		ChunkHash = HashCombine(
			ChunkHash,
			GetTypeHash(ToolSpline->GetLocationAtDistanceAlongSpline(
				Distance,
				ESplineCoordinateSpace::World)));
		ChunkHash = HashCombine(
			ChunkHash,
			GetTypeHash(ToolSpline->GetRightVectorAtDistanceAlongSpline(
				Distance,
				ESplineCoordinateSpace::World)));
	}

	return ChunkHash;
}

void AProceduralRoadActor::BuildCrossSections(
	float StartDistance,
	float EndDistance,
	TArray<FSplineRoadCrossSection>& OutCrossSections) const
{
	const float ChunkDistance = FMath::Max(EndDistance - StartDistance, 0.0f);
	const int32 SegmentCount = FMath::Max(
		FMath::CeilToInt(ChunkDistance / FMath::Max(SegmentLength, 10.0f)),
		1);
	const int32 RoadPointCount = FMath::Max(WidthSubdivisions, 1) + 1;
	const float DistanceStep = ChunkDistance / static_cast<float>(SegmentCount);
	const float WidthStep = RoadWidth / static_cast<float>(RoadPointCount - 1);
	const float HalfWidth = RoadWidth * 0.5f;

	OutCrossSections.Reserve(SegmentCount + 1);
	for (int32 SegmentIndex = 0; SegmentIndex <= SegmentCount; ++SegmentIndex)
	{
		FSplineRoadCrossSection& CrossSection = OutCrossSections.AddDefaulted_GetRef();
		CrossSection.DistanceAlongSpline = SegmentIndex == SegmentCount
			? EndDistance
			: StartDistance + DistanceStep * SegmentIndex;
		CrossSection.RoadPoints.Reserve(RoadPointCount);

		for (int32 WidthIndex = 0; WidthIndex < RoadPointCount; ++WidthIndex)
		{
			const float LateralOffset = -HalfWidth + WidthStep * WidthIndex;
			CrossSection.RoadPoints.Add(
				SampleRoadPosition(CrossSection.DistanceAlongSpline, LateralOffset));
		}

		CrossSection.LeftFlapPoint = SampleRoadPosition(
			CrossSection.DistanceAlongSpline,
			-HalfWidth - SideFlapWidth)
			- FVector::UpVector * SideFlapEmbedDepth;
		CrossSection.RightFlapPoint = SampleRoadPosition(
			CrossSection.DistanceAlongSpline,
			HalfWidth + SideFlapWidth)
			- FVector::UpVector * SideFlapEmbedDepth;
	}
}

void AProceduralRoadActor::BuildRoadSurface(
	UProceduralMeshComponent* RoadChunk,
	const TArray<FSplineRoadCrossSection>& CrossSections)
{
	const int32 RoadPointCount = CrossSections[0].RoadPoints.Num();
	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector2D> UVs;
	TArray<FVector2D> MarkingUVs;
	Vertices.Reserve(CrossSections.Num() * RoadPointCount);
	UVs.Reserve(CrossSections.Num() * RoadPointCount);
	MarkingUVs.Reserve(CrossSections.Num() * RoadPointCount);

	for (int32 SectionIndex = 0; SectionIndex < CrossSections.Num(); ++SectionIndex)
	{
		for (int32 WidthIndex = 0; WidthIndex < RoadPointCount; ++WidthIndex)
		{
			Vertices.Add(ToolSpline->GetComponentTransform().InverseTransformPosition(
				CrossSections[SectionIndex].RoadPoints[WidthIndex]));
			const float LateralDistance =
				RoadWidth * static_cast<float>(WidthIndex)
				/ static_cast<float>(RoadPointCount - 1);
			UVs.Add(FVector2D(
				LateralDistance / FMath::Max(UVWorldSize.X, 1.0f),
				CrossSections[SectionIndex].DistanceAlongSpline
					/ FMath::Max(UVWorldSize.Y, 1.0f)));
			MarkingUVs.Add(FVector2D(
				static_cast<float>(WidthIndex)
					/ static_cast<float>(RoadPointCount - 1),
				CrossSections[SectionIndex].DistanceAlongSpline
					/ FMath::Max(MarkingUVWorldLength, 1.0f)));
		}
	}

	for (int32 SectionIndex = 0; SectionIndex + 1 < CrossSections.Num(); ++SectionIndex)
	{
		for (int32 WidthIndex = 0; WidthIndex + 1 < RoadPointCount; ++WidthIndex)
		{
			const int32 FirstCurrent = SectionIndex * RoadPointCount + WidthIndex;
			const int32 FirstNext = (SectionIndex + 1) * RoadPointCount + WidthIndex;
			AddQuad(
				Triangles,
				FirstCurrent,
				FirstNext,
				FirstCurrent + 1,
				FirstNext + 1);
		}
	}

	CreateMeshSection(
		RoadChunk,
		0,
		Vertices,
		Triangles,
		UVs,
		MarkingUVs,
		RoadMaterial);
}

void AProceduralRoadActor::BuildSideFlaps(
	UProceduralMeshComponent* RoadChunk,
	const TArray<FSplineRoadCrossSection>& CrossSections)
{
	const bool bBuildSideFlaps = SideFlapWidth > KINDA_SMALL_NUMBER;
	const bool bBuildStartFlap =
		bGenerateEndFlaps
		&& !bClosedLoop
		&& EndFlapLength > KINDA_SMALL_NUMBER
		&& FMath::IsNearlyZero(CrossSections[0].DistanceAlongSpline);
	const bool bBuildEndFlap =
		bGenerateEndFlaps
		&& !bClosedLoop
		&& EndFlapLength > KINDA_SMALL_NUMBER
		&& FMath::IsNearlyEqual(
			CrossSections.Last().DistanceAlongSpline,
			ToolSpline->GetSplineLength());
	if (!bBuildSideFlaps && !bBuildStartFlap && !bBuildEndFlap)
	{
		return;
	}

	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector2D> UVs;
	if (bBuildSideFlaps)
	{
		Vertices.Reserve(CrossSections.Num() * 4);
		UVs.Reserve(CrossSections.Num() * 4);

		for (int32 SectionIndex = 0; SectionIndex < CrossSections.Num(); ++SectionIndex)
		{
			const FSplineRoadCrossSection& CrossSection = CrossSections[SectionIndex];
			const float V = CrossSection.DistanceAlongSpline / FMath::Max(UVWorldSize.Y, 1.0f);

			Vertices.Add(ToolSpline->GetComponentTransform().InverseTransformPosition(CrossSection.LeftFlapPoint));
			Vertices.Add(ToolSpline->GetComponentTransform().InverseTransformPosition(CrossSection.RoadPoints[0]));
			Vertices.Add(ToolSpline->GetComponentTransform().InverseTransformPosition(CrossSection.RoadPoints.Last()));
			Vertices.Add(ToolSpline->GetComponentTransform().InverseTransformPosition(CrossSection.RightFlapPoint));

			UVs.Add(FVector2D(0.0f, V));
			UVs.Add(FVector2D(SideFlapWidth / FMath::Max(UVWorldSize.X, 1.0f), V));
			UVs.Add(FVector2D(0.0f, V));
			UVs.Add(FVector2D(SideFlapWidth / FMath::Max(UVWorldSize.X, 1.0f), V));
		}

		for (int32 SectionIndex = 0; SectionIndex + 1 < CrossSections.Num(); ++SectionIndex)
		{
			const int32 Current = SectionIndex * 4;
			const int32 Next = (SectionIndex + 1) * 4;
			AddQuad(Triangles, Current, Next, Current + 1, Next + 1);
			AddQuad(Triangles, Current + 2, Next + 2, Current + 3, Next + 3);
		}
	}

	auto AddEndFlap = [&](const FSplineRoadCrossSection& CrossSection, bool bStart)
	{
		TArray<FVector> InnerPoints;
		if (bBuildSideFlaps)
		{
			InnerPoints.Add(CrossSection.LeftFlapPoint);
		}
		InnerPoints.Append(CrossSection.RoadPoints);
		if (bBuildSideFlaps)
		{
			InnerPoints.Add(CrossSection.RightFlapPoint);
		}

		FVector Direction = ToolSpline->GetTangentAtDistanceAlongSpline(
			CrossSection.DistanceAlongSpline,
			ESplineCoordinateSpace::World).GetSafeNormal();
		if (bStart)
		{
			Direction *= -1.0f;
		}

		const int32 OuterStart = Vertices.Num();
		for (int32 PointIndex = 0; PointIndex < InnerPoints.Num(); ++PointIndex)
		{
			const FVector DesiredPosition =
				InnerPoints[PointIndex] + Direction * EndFlapLength;
			FVector OuterPoint = DesiredPosition;
			FHitResult Hit;
			if (bAlignToTerrain && TraceTerrain(DesiredPosition, Hit))
			{
				OuterPoint =
					Hit.ImpactPoint
					+ Hit.ImpactNormal.GetSafeNormal() * RoadSurfaceOffset;
			}
			OuterPoint -= FVector::UpVector * EndFlapEmbedDepth;
			Vertices.Add(ToolSpline->GetComponentTransform().InverseTransformPosition(
				OuterPoint));

			const float U =
				static_cast<float>(PointIndex)
				/ static_cast<float>(InnerPoints.Num() - 1);
			UVs.Add(FVector2D(U, 0.0f));
		}

		const int32 InnerStart = Vertices.Num();
		for (int32 PointIndex = 0; PointIndex < InnerPoints.Num(); ++PointIndex)
		{
			Vertices.Add(ToolSpline->GetComponentTransform().InverseTransformPosition(
				InnerPoints[PointIndex]));
			const float U =
				static_cast<float>(PointIndex)
				/ static_cast<float>(InnerPoints.Num() - 1);
			UVs.Add(FVector2D(
				U,
				EndFlapLength / FMath::Max(UVWorldSize.Y, 1.0f)));
		}

		for (int32 PointIndex = 0; PointIndex + 1 < InnerPoints.Num(); ++PointIndex)
		{
			Triangles.Add(OuterStart + PointIndex);
			Triangles.Add(InnerStart + PointIndex);
			Triangles.Add(OuterStart + PointIndex + 1);
			Triangles.Add(OuterStart + PointIndex + 1);
			Triangles.Add(InnerStart + PointIndex);
			Triangles.Add(InnerStart + PointIndex + 1);
		}
	};

	if (bBuildStartFlap)
	{
		AddEndFlap(CrossSections[0], true);
	}
	if (bBuildEndFlap)
	{
		AddEndFlap(CrossSections.Last(), false);
	}

	CreateMeshSection(
		RoadChunk,
		1,
		Vertices,
		Triangles,
		UVs,
		TArray<FVector2D>(),
		SideFlapMaterial ? SideFlapMaterial.Get() : RoadMaterial.Get());
}

void AProceduralRoadActor::BuildRoadLines(
	UProceduralMeshComponent* RoadChunk,
	const TArray<FSplineRoadCrossSection>& CrossSections)
{
	if ((!bGenerateCenterLine && !bGenerateSideLines) || CrossSections.Num() < 2)
	{
		return;
	}

	auto AddLineRange = [&](
		TArray<FVector>& Vertices,
		TArray<int32>& Triangles,
		TArray<FVector2D>& UVs,
		float CenterOffset,
		float LineWidth,
		float SurfaceOffset,
		float UVWorldLength,
		float StartDistance,
		float EndDistance)
	{
		if (EndDistance - StartDistance <= KINDA_SMALL_NUMBER)
		{
			return;
		}

		TArray<float> SampleDistances;
		SampleDistances.Add(StartDistance);
		for (const FSplineRoadCrossSection& CrossSection : CrossSections)
		{
			if (CrossSection.DistanceAlongSpline > StartDistance
				&& CrossSection.DistanceAlongSpline < EndDistance)
			{
				SampleDistances.Add(CrossSection.DistanceAlongSpline);
			}
		}
		SampleDistances.Add(EndDistance);

		const int32 FirstVertex = Vertices.Num();
		const float HalfLineWidth = LineWidth * 0.5f;
		for (float Distance : SampleDistances)
		{
			Vertices.Add(ToolSpline->GetComponentTransform().InverseTransformPosition(
				SampleCrossSectionAtDistance(
					CrossSections,
					RoadWidth,
					Distance,
					CenterOffset - HalfLineWidth)
				+ FVector::UpVector * SurfaceOffset));
			Vertices.Add(ToolSpline->GetComponentTransform().InverseTransformPosition(
				SampleCrossSectionAtDistance(
					CrossSections,
					RoadWidth,
					Distance,
					CenterOffset + HalfLineWidth)
				+ FVector::UpVector * SurfaceOffset));

			const float V = Distance / FMath::Max(UVWorldLength, 1.0f);
			UVs.Add(FVector2D(0.0f, V));
			UVs.Add(FVector2D(1.0f, V));
		}

		for (int32 SampleIndex = 0; SampleIndex + 1 < SampleDistances.Num(); ++SampleIndex)
		{
			const int32 Current = FirstVertex + SampleIndex * 2;
			const int32 Next = Current + 2;
			AddQuad(Triangles, Current, Next, Current + 1, Next + 1);
		}
	};

	const float ChunkStartDistance = CrossSections[0].DistanceAlongSpline;
	const float ChunkEndDistance = CrossSections.Last().DistanceAlongSpline;

	if (bGenerateSideLines && RoadLineMaterial && RoadLineWidth < RoadWidth)
	{
		TArray<FVector> SideVertices;
		TArray<int32> SideTriangles;
		TArray<FVector2D> SideUVs;
		const float SideOffset = FMath::Clamp(
			RoadWidth * 0.5f - SideLineInset,
			RoadLineWidth * 0.5f,
			RoadWidth * 0.5f - RoadLineWidth * 0.5f);
		AddLineRange(
			SideVertices,
			SideTriangles,
			SideUVs,
			-SideOffset,
			RoadLineWidth,
			RoadLineSurfaceOffset,
			RoadLineUVWorldLength,
			ChunkStartDistance,
			ChunkEndDistance);
		AddLineRange(
			SideVertices,
			SideTriangles,
			SideUVs,
			SideOffset,
			RoadLineWidth,
			RoadLineSurfaceOffset,
			RoadLineUVWorldLength,
			ChunkStartDistance,
			ChunkEndDistance);
		CreateMeshSection(
			RoadChunk,
			2,
			SideVertices,
			SideTriangles,
			SideUVs,
			TArray<FVector2D>(),
			RoadLineMaterial);
	}

	if (bGenerateCenterLine
		&& GetCenterLineMaterial()
		&& CenterLineWidth < RoadWidth)
	{
		TArray<FVector> CenterVertices;
		TArray<int32> CenterTriangles;
		TArray<FVector2D> CenterUVs;
		if (!bCenterLineHasGaps)
		{
			AddLineRange(
				CenterVertices,
				CenterTriangles,
				CenterUVs,
				0.0f,
				CenterLineWidth,
				CenterLineSurfaceOffset,
				CenterLineUVWorldLength,
				ChunkStartDistance,
				ChunkEndDistance);
		}
		else
		{
			const float DashLength = FMath::Max(CenterLineDashLength, 1.0f);
			const float PatternLength =
				DashLength + FMath::Max(CenterLineGapLength, 1.0f);
			float PatternStart =
				FMath::FloorToFloat(ChunkStartDistance / PatternLength)
				* PatternLength;
			for (; PatternStart < ChunkEndDistance; PatternStart += PatternLength)
			{
				const float DashStart = FMath::Max(PatternStart, ChunkStartDistance);
				const float DashEnd = FMath::Min(
					PatternStart + DashLength,
					ChunkEndDistance);
				AddLineRange(
					CenterVertices,
					CenterTriangles,
					CenterUVs,
					0.0f,
					CenterLineWidth,
					CenterLineSurfaceOffset,
					CenterLineUVWorldLength,
					DashStart,
					DashEnd);
			}
		}

		if (!CenterVertices.IsEmpty())
		{
			CreateMeshSection(
				RoadChunk,
				3,
				CenterVertices,
				CenterTriangles,
				CenterUVs,
				TArray<FVector2D>(),
				GetCenterLineMaterial());
		}
	}
}

void AProceduralRoadActor::BuildSimpleCollision(
	UProceduralMeshComponent* RoadChunk,
	const TArray<FSplineRoadCrossSection>& CrossSections)
{
	if (!bGenerateCollision)
	{
		return;
	}

	const FVector LocalDown = ToolSpline->GetComponentTransform().InverseTransformVectorNoScale(
		-FVector::UpVector * CollisionThickness);
	const int32 SectionStride = FMath::Max(CollisionSegmentStride, 1);

	for (int32 StartIndex = 0; StartIndex + 1 < CrossSections.Num(); StartIndex += SectionStride)
	{
		const int32 EndIndex = FMath::Min(
			StartIndex + SectionStride,
			CrossSections.Num() - 1);
		TArray<FVector> ConvexVertices;
		ConvexVertices.Reserve((EndIndex - StartIndex + 1) * 4);

		for (int32 SectionIndex = StartIndex; SectionIndex <= EndIndex; ++SectionIndex)
		{
			const FVector Left = ToolSpline->GetComponentTransform().InverseTransformPosition(
				CrossSections[SectionIndex].RoadPoints[0]);
			const FVector Right = ToolSpline->GetComponentTransform().InverseTransformPosition(
				CrossSections[SectionIndex].RoadPoints.Last());
			ConvexVertices.Add(Left);
			ConvexVertices.Add(Right);
			ConvexVertices.Add(Left + LocalDown);
			ConvexVertices.Add(Right + LocalDown);
		}

		RoadChunk->AddCollisionConvexMesh(ConvexVertices);
	}
}

void AProceduralRoadActor::GenerateDecals()
{
	if (!bGenerateDecals || !DecalMaterial)
	{
		return;
	}

	const float SplineLength = ToolSpline->GetSplineLength();
	const float FirstDistance = FMath::Max(
		GetEffectiveStartDistance(),
		DecalStartPadding);
	const float LastDistance = FMath::Min(
		GetEffectiveEndDistance(),
		SplineLength - DecalEndPadding);
	if (LastDistance < FirstDistance)
	{
		return;
	}

	int32 DecalIndex = 0;
	for (float Distance = FirstDistance;
		Distance <= LastDistance;
		Distance += FMath::Max(DecalSpacing, 1.0f))
	{
		FVector SurfaceNormal = FVector::UpVector;
		const FVector Location = SampleRoadPosition(
			Distance,
			DecalLateralOffset,
			&SurfaceNormal);
		FVector Tangent = ToolSpline->GetTangentAtDistanceAlongSpline(
			Distance,
			ESplineCoordinateSpace::World).GetSafeNormal();
		Tangent = FVector::VectorPlaneProject(Tangent, SurfaceNormal).GetSafeNormal();
		if (Tangent.IsNearlyZero())
		{
			Tangent = FVector::ForwardVector;
		}

		const FTransform DecalTransform(
			FRotationMatrix::MakeFromXZ(-SurfaceNormal, Tangent).ToQuat()
				* DecalRotationOffset.Quaternion(),
			Location + SurfaceNormal * 2.0f);

		UDecalComponent* Decal = NewObject<UDecalComponent>(
			this,
			*FString::Printf(TEXT("RoadDecal_%d"), DecalIndex++));
		AddInstanceComponent(Decal);
		Decal->SetupAttachment(ToolSpline);
		Decal->SetDecalMaterial(DecalMaterial);
		Decal->DecalSize = DecalSize;
		Decal->FadeScreenSize = DecalFadeScreenSize;
		Decal->RegisterComponent();
		Decal->SetWorldTransform(DecalTransform);
		GeneratedDecals.Add(Decal);
	}
}

FVector AProceduralRoadActor::SampleRoadPosition(
	float DistanceAlongSpline,
	float LateralOffset,
	FVector* OutSurfaceNormal) const
{
	const FVector SplineLocation = ToolSpline->GetLocationAtDistanceAlongSpline(
		DistanceAlongSpline,
		ESplineCoordinateSpace::World);
	const FVector RightVector = ToolSpline->GetRightVectorAtDistanceAlongSpline(
		DistanceAlongSpline,
		ESplineCoordinateSpace::World).GetSafeNormal();
	const FVector DesiredPosition = SplineLocation + RightVector * LateralOffset;

	if (OutSurfaceNormal)
	{
		*OutSurfaceNormal = ToolSpline->GetUpVectorAtDistanceAlongSpline(
			DistanceAlongSpline,
			ESplineCoordinateSpace::World).GetSafeNormal();
	}
	if (!bAlignToTerrain)
	{
		return DesiredPosition;
	}

	FHitResult Hit;
	if (!TraceTerrain(DesiredPosition, Hit))
	{
		return DesiredPosition;
	}

	if (OutSurfaceNormal)
	{
		*OutSurfaceNormal = Hit.ImpactNormal.GetSafeNormal();
	}
	return Hit.ImpactPoint + Hit.ImpactNormal.GetSafeNormal() * RoadSurfaceOffset;
}

bool AProceduralRoadActor::TraceTerrain(
	const FVector& DesiredPosition,
	FHitResult& OutHit) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	FCollisionQueryParams QueryParams(
		SCENE_QUERY_STAT(ProceduralRoadTerrain),
		false,
		this);
	TArray<FHitResult> Hits;
	World->LineTraceMultiByChannel(
		Hits,
		DesiredPosition + FVector::UpVector * TerrainTraceHeightAbove,
		DesiredPosition - FVector::UpVector * TerrainTraceHeightBelow,
		TerrainTraceChannel,
		QueryParams);
	for (const FHitResult& Hit : Hits)
	{
		if (Hit.GetActor()
			&& !Hit.GetActor()->IsA<AProceduralRoadActor>()
			&& !Hit.GetActor()->IsA<AProceduralRoadJunctionActor>())
		{
			OutHit = Hit;
			return true;
		}
	}
	return false;
}

UMaterialInterface* AProceduralRoadActor::GetCenterLineMaterial() const
{
	if (CenterLineMaterial)
	{
		return CenterLineMaterial;
	}

	return RoadLineMaterial;
}

float AProceduralRoadActor::GetEffectiveStartDistance() const
{
	float TrimDistance = 0.0f;
	if (StartJunctionOwner.IsValid())
	{
		TrimDistance = StartJunctionTrimDistance;
	}
	return FMath::Clamp(
		TrimDistance,
		0.0f,
		ToolSpline->GetSplineLength());
}

float AProceduralRoadActor::GetEffectiveEndDistance() const
{
	float TrimDistance = 0.0f;
	if (EndJunctionOwner.IsValid())
	{
		TrimDistance = EndJunctionTrimDistance;
	}
	return FMath::Clamp(
		ToolSpline->GetSplineLength() - TrimDistance,
		0.0f,
		ToolSpline->GetSplineLength());
}

void AProceduralRoadActor::CreateMeshSection(
	UProceduralMeshComponent* RoadChunk,
	int32 SectionIndex,
	const TArray<FVector>& Vertices,
	const TArray<int32>& Triangles,
	const TArray<FVector2D>& UVs,
	const TArray<FVector2D>& MarkingUVs,
	UMaterialInterface* Material)
{
	TArray<int32> UpwardTriangles = Triangles;
	const FVector LocalWorldUp = ToolSpline->GetComponentTransform().InverseTransformVectorNoScale(
		FVector::UpVector);
	for (int32 TriangleIndex = 0; TriangleIndex + 2 < UpwardTriangles.Num(); TriangleIndex += 3)
	{
		const FVector& First = Vertices[UpwardTriangles[TriangleIndex]];
		const FVector& Second = Vertices[UpwardTriangles[TriangleIndex + 1]];
		const FVector& Third = Vertices[UpwardTriangles[TriangleIndex + 2]];
		const FVector TriangleNormal = FVector::CrossProduct(
			Second - First,
			Third - First);
		// ProceduralMesh uses Unreal's clockwise front-face winding. Its tangent
		// helper derives the rendered normal opposite to this conventional cross product.
		if (FVector::DotProduct(TriangleNormal, LocalWorldUp) > 0.0f)
		{
			Swap(
				UpwardTriangles[TriangleIndex + 1],
				UpwardTriangles[TriangleIndex + 2]);
		}
	}

	TArray<FVector> Normals;
	TArray<FProcMeshTangent> Tangents;
	UKismetProceduralMeshLibrary::CalculateTangentsForMesh(
		Vertices,
		UpwardTriangles,
		UVs,
		Normals,
		Tangents);

	TArray<FLinearColor> VertexColors;
	TArray<FVector2D> EmptyUVs;
	RoadChunk->CreateMeshSection_LinearColor(
		SectionIndex,
		Vertices,
		UpwardTriangles,
		Normals,
		UVs,
		MarkingUVs,
		EmptyUVs,
		EmptyUVs,
		VertexColors,
		Tangents,
		false);
	RoadChunk->SetMaterial(SectionIndex, Material);
}
