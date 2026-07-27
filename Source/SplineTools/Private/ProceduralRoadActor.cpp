#include "ProceduralRoadActor.h"

#include "Components/DecalComponent.h"
#include "Components/SplineComponent.h"
#include "Engine/World.h"
#include "KismetProceduralMeshLibrary.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"

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

void AProceduralRoadActor::RebuildSplineTool()
{
	UpdateSplineSettings();
	if (!IsSplineUsable())
	{
		ResetGeneratedContent();
		return;
	}

	UpdateRoadChunks();
	ResetGeneratedDecals();
	GenerateDecals();
}

void AProceduralRoadActor::RebuildRoad()
{
	RebuildSplineTool();
}

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
	RoadChunkHashes.Reset();

	ResetGeneratedDecals();
	Super::ResetGeneratedContent();
}

void AProceduralRoadActor::ResetGeneratedDecals()
{
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

	const float SplineLength = ToolSpline->GetSplineLength();
	const int32 ChunkCount = FMath::Max(
		FMath::CeilToInt(SplineLength / FMath::Max(ChunkLength, 100.0f)),
		1);
	EnsureRoadChunkCount(ChunkCount);
	RoadChunkHashes.SetNum(ChunkCount);

	for (int32 ChunkIndex = 0; ChunkIndex < ChunkCount; ++ChunkIndex)
	{
		const float StartDistance = FMath::Min(
			ChunkIndex * FMath::Max(ChunkLength, 100.0f),
			SplineLength);
		const float EndDistance = ChunkIndex == ChunkCount - 1
			? SplineLength
			: FMath::Min(
				(ChunkIndex + 1) * FMath::Max(ChunkLength, 100.0f),
				SplineLength);

		TArray<FSplineRoadCrossSection> CrossSections;
		BuildCrossSections(StartDistance, EndDistance, CrossSections);
		const uint32 ChunkHash = CalculateChunkHash(CrossSections);
		UProceduralMeshComponent* RoadChunk = GeneratedRoadChunks[ChunkIndex];
		ConfigureRoadChunk(RoadChunk);
		RoadChunk->SetMaterial(0, RoadMaterial);
		RoadChunk->SetMaterial(
			1,
			SideFlapMaterial ? SideFlapMaterial.Get() : RoadMaterial.Get());

		if (RoadChunkHashes[ChunkIndex] == ChunkHash
			&& RoadChunk->GetNumSections() > 0)
		{
			continue;
		}

		RebuildRoadChunk(RoadChunk, CrossSections);
		RoadChunkHashes[ChunkIndex] = ChunkHash;
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
	BuildSimpleCollision(RoadChunk, CrossSections);
	RoadChunk->MarkRenderStateDirty();
}

uint32 AProceduralRoadActor::CalculateChunkHash(
	const TArray<FSplineRoadCrossSection>& CrossSections) const
{
	uint32 ChunkHash = GetTypeHash(RoadWidth);
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(SegmentLength));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(WidthSubdivisions));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(UVWorldSize));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(SideFlapWidth));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(SideFlapEmbedDepth));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(bGenerateCollision));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(CollisionThickness));
	ChunkHash = HashCombine(ChunkHash, GetTypeHash(CollisionSegmentStride));

	for (const FSplineRoadCrossSection& CrossSection : CrossSections)
	{
		ChunkHash = HashCombine(
			ChunkHash,
			GetTypeHash(CrossSection.DistanceAlongSpline));
		for (const FVector& RoadPoint : CrossSection.RoadPoints)
		{
			ChunkHash = HashCombine(ChunkHash, GetTypeHash(RoadPoint));
		}
		ChunkHash = HashCombine(ChunkHash, GetTypeHash(CrossSection.LeftFlapPoint));
		ChunkHash = HashCombine(ChunkHash, GetTypeHash(CrossSection.RightFlapPoint));
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
	Vertices.Reserve(CrossSections.Num() * RoadPointCount);
	UVs.Reserve(CrossSections.Num() * RoadPointCount);

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

	CreateMeshSection(RoadChunk, 0, Vertices, Triangles, UVs, RoadMaterial);
}

void AProceduralRoadActor::BuildSideFlaps(
	UProceduralMeshComponent* RoadChunk,
	const TArray<FSplineRoadCrossSection>& CrossSections)
{
	if (SideFlapWidth <= KINDA_SMALL_NUMBER)
	{
		return;
	}

	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector2D> UVs;
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

	CreateMeshSection(
		RoadChunk,
		1,
		Vertices,
		Triangles,
		UVs,
		SideFlapMaterial ? SideFlapMaterial.Get() : RoadMaterial.Get());
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
	const float LastDistance = SplineLength - DecalEndPadding;
	if (LastDistance < DecalStartPadding)
	{
		return;
	}

	int32 DecalIndex = 0;
	for (float Distance = DecalStartPadding;
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

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(ProceduralRoadTerrain), false, this);
	return World->LineTraceSingleByChannel(
		OutHit,
		DesiredPosition + FVector::UpVector * TerrainTraceHeightAbove,
		DesiredPosition - FVector::UpVector * TerrainTraceHeightBelow,
		TerrainTraceChannel,
		QueryParams);
}

void AProceduralRoadActor::CreateMeshSection(
	UProceduralMeshComponent* RoadChunk,
	int32 SectionIndex,
	const TArray<FVector>& Vertices,
	const TArray<int32>& Triangles,
	const TArray<FVector2D>& UVs,
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
	RoadChunk->CreateMeshSection_LinearColor(
		SectionIndex,
		Vertices,
		UpwardTriangles,
		Normals,
		UVs,
		VertexColors,
		Tangents,
		false);
	RoadChunk->SetMaterial(SectionIndex, Material);
}
