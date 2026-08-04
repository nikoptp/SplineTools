#include "ProceduralRoadJunctionActor.h"

#include "Components/SceneComponent.h"
#include "Components/SphereComponent.h"
#include "Containers/Ticker.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "KismetProceduralMeshLibrary.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"
#include "UObject/UnrealType.h"

AProceduralRoadJunctionActor::AProceduralRoadJunctionActor()
{
	PrimaryActorTick.bCanEverTick = false;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	JunctionMesh = CreateDefaultSubobject<UProceduralMeshComponent>(
		TEXT("JunctionMesh"));
	JunctionMesh->SetupAttachment(SceneRoot);
	JunctionMesh->SetMobility(EComponentMobility::Movable);
	JunctionMesh->bUseAsyncCooking = true;
	JunctionMesh->bUseComplexAsSimpleCollision = false;
	JunctionMesh->SetCollisionProfileName(TEXT("BlockAll"));

	DiscoveryPreview = CreateDefaultSubobject<USphereComponent>(
		TEXT("EndpointDiscoveryPreview"));
	DiscoveryPreview->SetupAttachment(SceneRoot);
	DiscoveryPreview->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	DiscoveryPreview->SetGenerateOverlapEvents(false);
	DiscoveryPreview->SetCanEverAffectNavigation(false);
	DiscoveryPreview->SetHiddenInGame(true);
	DiscoveryPreview->ShapeColor = FColor::Orange;
	DiscoveryPreview->bIsEditorOnly = true;
}

void AProceduralRoadJunctionActor::BeginPlay()
{
	Super::BeginPlay();
}

void AProceduralRoadJunctionActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	if (GetWorld() && GetWorld()->IsGameWorld())
	{
		return;
	}

#if WITH_EDITOR
	QueueEditorRebuild();
#endif
}

void AProceduralRoadJunctionActor::Destroyed()
{
#if WITH_EDITOR
	CancelQueuedEditorRebuild();
#endif
	ReleaseRoadTrims();
	Super::Destroyed();
}

void AProceduralRoadJunctionActor::RebuildJunction()
{
	DiscoverNearbyRoadEndpoints();
	SynchronizeRoadTrims();
	JunctionMesh->ClearAllMeshSections();
	JunctionMesh->ClearCollisionConvexMeshes();
	UpdateEditorVisualization(GenerateJunctionPatch());
}

#if WITH_EDITOR
void AProceduralRoadJunctionActor::SetManagedConnections(
	const TArray<FProceduralRoadJunctionConnection>& InConnections,
	const FGuid& NetworkId,
	const FGuid& NodeId,
	bool bRebuild)
{
	Modify();
	ReleaseRoadTrims();
	bAutoDiscoverRoadEndpoints = false;
	Connections = InConnections;
	ManagedRoadNetworkId = NetworkId;
	ManagedRoadNodeId = NodeId;
	if (bRebuild)
	{
		RebuildJunction();
	}
}

bool AProceduralRoadJunctionActor::IsManagedByRoadNetwork(
	const FGuid& NetworkId) const
{
	return NetworkId.IsValid() && ManagedRoadNetworkId == NetworkId;
}

FGuid AProceduralRoadJunctionActor::GetManagedRoadNodeId() const
{
	return ManagedRoadNodeId;
}

const TArray<FProceduralRoadJunctionConnection>&
AProceduralRoadJunctionActor::GetRoadConnections() const
{
	return Connections;
}
#endif

void AProceduralRoadJunctionActor::DiscoverNearbyRoadEndpoints()
{
	if (!bAutoDiscoverRoadEndpoints || !GetWorld())
	{
		return;
	}

	struct FEndpointCandidate
	{
		TObjectPtr<AProceduralRoadActor> Road;
		ERoadSplineEndpoint Endpoint = ERoadSplineEndpoint::End;
		float DistanceSquared = 0.0f;
	};

	TArray<FEndpointCandidate> Candidates;
	const float SearchRadiusSquared = FMath::Square(
		FMath::Max(EndpointSearchRadius, 1.0f));
	for (TActorIterator<AProceduralRoadActor> RoadIterator(GetWorld());
		RoadIterator;
		++RoadIterator)
	{
		AProceduralRoadActor* Road = *RoadIterator;
		for (ERoadSplineEndpoint Endpoint :
			{ERoadSplineEndpoint::Start, ERoadSplineEndpoint::End})
		{
			FVector EndpointLocation;
			if (!Road->GetSplineEndpointLocation(Endpoint, EndpointLocation)
				|| !Road->IsJunctionEndpointAvailable(Endpoint, this))
			{
				continue;
			}

			const float DistanceSquared = FVector::DistSquared(
				GetActorLocation(),
				EndpointLocation);
			if (DistanceSquared > SearchRadiusSquared)
			{
				continue;
			}

			FEndpointCandidate& Candidate = Candidates.AddDefaulted_GetRef();
			Candidate.Road = Road;
			Candidate.Endpoint = Endpoint;
			Candidate.DistanceSquared = DistanceSquared;
		}
	}

	Candidates.Sort(
		[](const FEndpointCandidate& First, const FEndpointCandidate& Second)
		{
			if (!FMath::IsNearlyEqual(
				First.DistanceSquared,
				Second.DistanceSquared))
			{
				return First.DistanceSquared < Second.DistanceSquared;
			}

			const int32 NameComparison = First.Road->GetFName().Compare(
				Second.Road->GetFName());
			if (NameComparison != 0)
			{
				return NameComparison < 0;
			}
			return First.Endpoint < Second.Endpoint;
		});

	Connections.Reset(Candidates.Num());
	for (const FEndpointCandidate& Candidate : Candidates)
	{
		FProceduralRoadJunctionConnection& Connection =
			Connections.AddDefaulted_GetRef();
		Connection.Road = Candidate.Road;
		Connection.Endpoint = Candidate.Endpoint;
		Connection.TrimDistance = AutomaticTrimDistance;
	}
}

void AProceduralRoadJunctionActor::SynchronizeRoadTrims()
{
	for (int32 AppliedIndex = AppliedConnections.Num() - 1;
		AppliedIndex >= 0;
		--AppliedIndex)
	{
		if (IsConnectionConfigured(AppliedConnections[AppliedIndex]))
		{
			continue;
		}

		if (AppliedConnections[AppliedIndex].Road.IsValid())
		{
			AppliedConnections[AppliedIndex].Road->ClearJunctionTrim(
				AppliedConnections[AppliedIndex].Endpoint,
				this);
		}
		AppliedConnections.RemoveAtSwap(AppliedIndex);
	}

	for (const FProceduralRoadJunctionConnection& Connection : Connections)
	{
		if (!Connection.Road
			|| !Connection.Road->SetJunctionTrim(
				Connection.Endpoint,
				Connection.TrimDistance,
				this))
		{
			continue;
		}

		bool bAlreadyApplied = false;
		for (const FAppliedConnection& AppliedConnection : AppliedConnections)
		{
			if (AppliedConnection.Road == Connection.Road
				&& AppliedConnection.Endpoint == Connection.Endpoint)
			{
				bAlreadyApplied = true;
				break;
			}
		}
		if (!bAlreadyApplied)
		{
			FAppliedConnection& AppliedConnection =
				AppliedConnections.AddDefaulted_GetRef();
			AppliedConnection.Road = Connection.Road;
			AppliedConnection.Endpoint = Connection.Endpoint;
		}
	}
}

void AProceduralRoadJunctionActor::ReleaseRoadTrims()
{
	for (const FAppliedConnection& AppliedConnection : AppliedConnections)
	{
		if (AppliedConnection.Road.IsValid())
		{
			AppliedConnection.Road->ClearJunctionTrim(
				AppliedConnection.Endpoint,
				this);
		}
	}
	AppliedConnections.Reset();
}

bool AProceduralRoadJunctionActor::IsConnectionConfigured(
	const FAppliedConnection& AppliedConnection) const
{
	for (const FProceduralRoadJunctionConnection& Connection : Connections)
	{
		if (AppliedConnection.Road == Connection.Road
			&& AppliedConnection.Endpoint == Connection.Endpoint)
		{
			return true;
		}
	}
	return false;
}

UMaterialInterface* AProceduralRoadJunctionActor::GetEffectiveJunctionMaterial() const
{
	if (JunctionMaterial)
	{
		return JunctionMaterial;
	}

	TArray<UMaterialInterface*> Materials;
	TArray<int32> MaterialCounts;
	for (const FProceduralRoadJunctionConnection& Connection : Connections)
	{
		if (!Connection.Road)
		{
			continue;
		}

		UMaterialInterface* RoadMaterial = Connection.Road->GetRoadMaterial();
		if (!RoadMaterial)
		{
			continue;
		}

		const int32 MaterialIndex = Materials.IndexOfByKey(RoadMaterial);
		if (MaterialIndex == INDEX_NONE)
		{
			Materials.Add(RoadMaterial);
			MaterialCounts.Add(1);
		}
		else
		{
			MaterialCounts[MaterialIndex]++;
		}
	}

	int32 MostCommonMaterialIndex = INDEX_NONE;
	for (int32 MaterialIndex = 0; MaterialIndex < MaterialCounts.Num(); ++MaterialIndex)
	{
		if (MostCommonMaterialIndex == INDEX_NONE
			|| MaterialCounts[MaterialIndex] > MaterialCounts[MostCommonMaterialIndex])
		{
			MostCommonMaterialIndex = MaterialIndex;
		}
	}

	return MostCommonMaterialIndex != INDEX_NONE
		? Materials[MostCommonMaterialIndex]
		: nullptr;
}

bool AProceduralRoadJunctionActor::GenerateJunctionPatch()
{
	TArray<FVector> BoundaryPoints;
	for (const FProceduralRoadJunctionConnection& Connection : Connections)
	{
		if (!Connection.Road)
		{
			continue;
		}

		bool bConnectionApplied = false;
		for (const FAppliedConnection& AppliedConnection : AppliedConnections)
		{
			if (AppliedConnection.Road == Connection.Road
				&& AppliedConnection.Endpoint == Connection.Endpoint)
			{
				bConnectionApplied = true;
				break;
			}
		}
		if (!bConnectionApplied)
		{
			continue;
		}

		FVector Center;
		FVector Left;
		FVector Right;
		FVector Direction;
		if (!Connection.Road->GetJunctionEdge(
			Connection.Endpoint,
			Connection.TrimDistance,
			Center,
			Left,
			Right,
			Direction))
		{
			continue;
		}

		BoundaryPoints.Add(Left);
		BoundaryPoints.Add(Right);
	}

	if (BoundaryPoints.Num() < 3)
	{
		JunctionMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		return false;
	}

	const FVector Center = ProjectToTerrain(GetActorLocation());
	BoundaryPoints.Sort(
		[Center](const FVector& First, const FVector& Second)
		{
			const float FirstAngle = FMath::Atan2(
				First.Y - Center.Y,
				First.X - Center.X);
			const float SecondAngle = FMath::Atan2(
				Second.Y - Center.Y,
				Second.X - Center.X);
			return FirstAngle < SecondAngle;
		});

	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector2D> UVs;
	Vertices.Reserve(BoundaryPoints.Num() + 1);
	UVs.Reserve(BoundaryPoints.Num() + 1);

	Vertices.Add(GetActorTransform().InverseTransformPosition(Center));
	UVs.Add(FVector2D(
		Center.X / FMath::Max(UVWorldSize.X, 1.0f),
		Center.Y / FMath::Max(UVWorldSize.Y, 1.0f)));
	for (const FVector& BoundaryPoint : BoundaryPoints)
	{
		Vertices.Add(GetActorTransform().InverseTransformPosition(BoundaryPoint));
		UVs.Add(FVector2D(
			BoundaryPoint.X / FMath::Max(UVWorldSize.X, 1.0f),
			BoundaryPoint.Y / FMath::Max(UVWorldSize.Y, 1.0f)));
	}

	for (int32 PointIndex = 0; PointIndex < BoundaryPoints.Num(); ++PointIndex)
	{
		const int32 FirstBoundaryIndex = PointIndex + 1;
		const int32 SecondBoundaryIndex =
			((PointIndex + 1) % BoundaryPoints.Num()) + 1;
		Triangles.Add(0);
		Triangles.Add(FirstBoundaryIndex);
		Triangles.Add(SecondBoundaryIndex);
	}

	const FVector LocalWorldUp = GetActorTransform().InverseTransformVectorNoScale(
		FVector::UpVector);
	for (int32 TriangleIndex = 0; TriangleIndex + 2 < Triangles.Num(); TriangleIndex += 3)
	{
		const FVector& First = Vertices[Triangles[TriangleIndex]];
		const FVector& Second = Vertices[Triangles[TriangleIndex + 1]];
		const FVector& Third = Vertices[Triangles[TriangleIndex + 2]];
		const FVector TriangleNormal = FVector::CrossProduct(
			Second - First,
			Third - First);
		if (FVector::DotProduct(TriangleNormal, LocalWorldUp) > 0.0f)
		{
			Swap(Triangles[TriangleIndex + 1], Triangles[TriangleIndex + 2]);
		}
	}

	TArray<FVector> Normals;
	TArray<FProcMeshTangent> Tangents;
	UKismetProceduralMeshLibrary::CalculateTangentsForMesh(
		Vertices,
		Triangles,
		UVs,
		Normals,
		Tangents);
	TArray<FLinearColor> VertexColors;
	JunctionMesh->CreateMeshSection_LinearColor(
		0,
		Vertices,
		Triangles,
		Normals,
		UVs,
		VertexColors,
		Tangents,
		false);
	JunctionMesh->SetMaterial(0, GetEffectiveJunctionMaterial());

	if (bGenerateCollision)
	{
		TArray<FVector> ConvexVertices;
		const FVector LocalDown = GetActorTransform().InverseTransformVectorNoScale(
			-FVector::UpVector * CollisionThickness);
		for (int32 PointIndex = 1; PointIndex < Vertices.Num(); ++PointIndex)
		{
			ConvexVertices.Add(Vertices[PointIndex]);
			ConvexVertices.Add(Vertices[PointIndex] + LocalDown);
		}
		JunctionMesh->AddCollisionConvexMesh(ConvexVertices);
	}

	JunctionMesh->SetCollisionEnabled(
		bGenerateCollision
			? ECollisionEnabled::QueryAndPhysics
			: ECollisionEnabled::NoCollision);
	JunctionMesh->MarkRenderStateDirty();
	return true;
}

void AProceduralRoadJunctionActor::UpdateEditorVisualization(
	bool bHasGeneratedMesh)
{
	DiscoveryPreview->SetSphereRadius(
		FMath::Max(EndpointSearchRadius, 1.0f));
	DiscoveryPreview->ShapeColor = AppliedConnections.Num() >= 2
		? FColor::Green
		: FColor::Orange;
	DiscoveryPreview->SetVisibility(!bHasGeneratedMesh);
	DiscoveryPreview->MarkRenderStateDirty();
}

bool AProceduralRoadJunctionActor::TraceTerrain(
	const FVector& DesiredPosition,
	FHitResult& OutHit) const
{
	if (!GetWorld())
	{
		return false;
	}

	FCollisionQueryParams QueryParams(
		SCENE_QUERY_STAT(ProceduralRoadJunctionTerrain),
		false,
		this);
	return GetWorld()->LineTraceSingleByChannel(
		OutHit,
		DesiredPosition + FVector::UpVector * TerrainTraceHeightAbove,
		DesiredPosition - FVector::UpVector * TerrainTraceHeightBelow,
		TerrainTraceChannel,
		QueryParams);
}

FVector AProceduralRoadJunctionActor::ProjectToTerrain(
	const FVector& DesiredPosition) const
{
	if (!bAlignToTerrain)
	{
		return DesiredPosition;
	}

	FHitResult Hit;
	if (!TraceTerrain(DesiredPosition, Hit))
	{
		return DesiredPosition;
	}

	return Hit.ImpactPoint + Hit.ImpactNormal.GetSafeNormal() * SurfaceOffset;
}

#if WITH_EDITOR
void AProceduralRoadJunctionActor::NotifyRoadEdited(
	AProceduralRoadActor* Road)
{
	if (!Road
		|| !Road->GetWorld()
		|| Road->GetWorld()->IsGameWorld())
	{
		return;
	}

	for (TActorIterator<AProceduralRoadJunctionActor> JunctionIterator(
		Road->GetWorld());
		JunctionIterator;
		++JunctionIterator)
	{
		JunctionIterator->QueueEditorRebuild();
	}
}

void AProceduralRoadJunctionActor::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);
	if (bFinished)
	{
		CancelQueuedEditorRebuild();
		RebuildJunction();
		return;
	}

	QueueEditorRebuild();
}

void AProceduralRoadJunctionActor::PostEditChangeProperty(
	FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	QueueEditorRebuild();
}

void AProceduralRoadJunctionActor::PostEditUndo()
{
	Super::PostEditUndo();
	QueueEditorRebuild();
}

void AProceduralRoadJunctionActor::QueueEditorRebuild()
{
	CancelQueuedEditorRebuild();
	EditorRebuildTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateWeakLambda(
			this,
			[this](float)
			{
				EditorRebuildTickerHandle.Reset();
				if (!IsTemplate())
				{
					RebuildJunction();
				}
				return false;
			}),
		0.15f);
}

void AProceduralRoadJunctionActor::CancelQueuedEditorRebuild()
{
	if (!EditorRebuildTickerHandle.IsValid())
	{
		return;
	}

	FTSTicker::GetCoreTicker().RemoveTicker(EditorRebuildTickerHandle);
	EditorRebuildTickerHandle.Reset();
}
#endif
