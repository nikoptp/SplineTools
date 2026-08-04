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
	RebuildJunctionInternal(true);
}

void AProceduralRoadJunctionActor::RebuildJunctionInternal(bool bMarkDirty)
{
#if WITH_EDITOR
	const bool bIsEditorWorld = !GetWorld() || !GetWorld()->IsGameWorld();
	if (bMarkDirty && bIsEditorWorld)
	{
		CancelQueuedEditorRebuild();
		Modify();
		JunctionMesh->Modify();
	}
#endif
	DiscoverNearbyRoadEndpoints();
	SynchronizeRoadTrims();
	RebuildJunctionMesh();
#if WITH_EDITOR
	if (bMarkDirty && bIsEditorWorld)
	{
		MarkPackageDirty();
	}
#endif
}

void AProceduralRoadJunctionActor::RebuildJunctionMesh()
{
	JunctionMesh->ClearAllMeshSections();
	JunctionMesh->ClearCollisionConvexMeshes();
	UpdateEditorVisualization(GenerateJunctionPatch());
}

#if WITH_EDITOR
void AProceduralRoadJunctionActor::SynchronizeJunctionRoadTrims()
{
	DiscoverNearbyRoadEndpoints();
	SynchronizeRoadTrims();
}

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

float AProceduralRoadJunctionActor::GetNearbyJunctionSearchRadius() const
{
	return NearbyJunctionSearchRadius;
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
		const float EffectiveTrimDistance = GetEffectiveTrimDistance(Connection);
		if (!Connection.Road
			|| !Connection.Road->SetJunctionTrim(
				Connection.Endpoint,
				EffectiveTrimDistance,
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
			AppliedConnection.TrimDistance = EffectiveTrimDistance;
		}
		else
		{
			for (FAppliedConnection& AppliedConnection : AppliedConnections)
			{
				if (AppliedConnection.Road == Connection.Road
					&& AppliedConnection.Endpoint == Connection.Endpoint)
				{
					AppliedConnection.TrimDistance = EffectiveTrimDistance;
					break;
				}
			}
		}
	}
}

float AProceduralRoadJunctionActor::GetEffectiveTrimDistance(
	const FProceduralRoadJunctionConnection& Connection) const
{
	if (!Connection.Road || !GetWorld())
	{
		return FMath::Max(Connection.TrimDistance, 0.0f);
	}

	float PairedTrimDistance = 0.0f;
	bool bHasPairedJunction = false;
	for (TActorIterator<AProceduralRoadJunctionActor> JunctionIterator(GetWorld());
		JunctionIterator;
		++JunctionIterator)
	{
		if (*JunctionIterator == this)
		{
			continue;
		}
		for (const FProceduralRoadJunctionConnection& OtherConnection :
			JunctionIterator->Connections)
		{
			if (OtherConnection.Road == Connection.Road
				&& OtherConnection.Endpoint != Connection.Endpoint)
			{
				PairedTrimDistance = FMath::Max(OtherConnection.TrimDistance, 0.0f);
				bHasPairedJunction = true;
				break;
			}
		}
		if (bHasPairedJunction)
		{
			break;
		}
	}

	const float RequestedTrimDistance = FMath::Max(Connection.TrimDistance, 0.0f);
	if (!bHasPairedJunction)
	{
		return RequestedTrimDistance;
	}

	const float AvailableTrimDistance = FMath::Max(
		0.0f,
		Connection.Road->GetRoadSplineLength() - MinimumRoadLengthBetweenJunctions);
	const float CombinedRequestedTrim = RequestedTrimDistance + PairedTrimDistance;
	return CombinedRequestedTrim > AvailableTrimDistance
		&& CombinedRequestedTrim > KINDA_SMALL_NUMBER
		? AvailableTrimDistance * RequestedTrimDistance / CombinedRequestedTrim
		: RequestedTrimDistance;
}

void AProceduralRoadJunctionActor::GatherNearbyJunctions(
	TArray<AProceduralRoadJunctionActor*>& OutJunctions) const
{
	OutJunctions.Reset();
	if (!GetWorld() || NearbyJunctionSearchRadius <= KINDA_SMALL_NUMBER)
	{
		return;
	}

	const float SearchRadiusSquared = FMath::Square(NearbyJunctionSearchRadius);
	for (TActorIterator<AProceduralRoadJunctionActor> JunctionIterator(GetWorld());
		JunctionIterator;
		++JunctionIterator)
	{
		AProceduralRoadJunctionActor* Junction = *JunctionIterator;
		if (Junction != this
			&& FVector2D::DistSquared(
				FVector2D(GetActorLocation()),
				FVector2D(Junction->GetActorLocation())) <= SearchRadiusSquared)
		{
			OutJunctions.Add(Junction);
		}
	}
}

FVector AProceduralRoadJunctionActor::ClampBlendPointToNearbyJunctions(
	const FVector& InnerPoint,
	const FVector& DesiredOuterPoint,
	const TArray<AProceduralRoadJunctionActor*>& NearbyJunctions) const
{
	const FVector2D SelfCenter(GetActorLocation());
	const FVector2D Inner(InnerPoint);
	const FVector2D Extension(
		DesiredOuterPoint.X - InnerPoint.X,
		DesiredOuterPoint.Y - InnerPoint.Y);
	float MaximumAlpha = 1.0f;
	for (const AProceduralRoadJunctionActor* NearbyJunction : NearbyJunctions)
	{
		const FVector2D OtherCenter(NearbyJunction->GetActorLocation());
		const FVector2D CenterDirection = OtherCenter - SelfCenter;
		const float Denominator = 2.0f * FVector2D::DotProduct(
			Extension,
			CenterDirection);
		if (Denominator <= KINDA_SMALL_NUMBER)
		{
			continue;
		}
		const float Numerator = OtherCenter.SizeSquared()
			- SelfCenter.SizeSquared()
			- 2.0f * FVector2D::DotProduct(Inner, CenterDirection);
		MaximumAlpha = FMath::Min(MaximumAlpha, Numerator / Denominator);
	}
	MaximumAlpha = FMath::Clamp(MaximumAlpha, 0.0f, 1.0f);
	return FMath::Lerp(InnerPoint, DesiredOuterPoint, MaximumAlpha);
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

UMaterialInterface* AProceduralRoadJunctionActor::GetEffectiveGroundBlendMaterial() const
{
	TArray<UMaterialInterface*> Materials;
	TArray<int32> MaterialCounts;
	for (const FProceduralRoadJunctionConnection& Connection : Connections)
	{
		if (!Connection.Road)
		{
			continue;
		}

		UMaterialInterface* RoadMaterial =
			Connection.Road->GetRoadSideFlapMaterial();
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
		: GetEffectiveJunctionMaterial();
}

bool AProceduralRoadJunctionActor::GenerateJunctionPatch()
{
	if (AppliedConnections.Num() < 2)
	{
		JunctionMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		return false;
	}

	struct FBoundarySample
	{
		FVector Location = FVector::ZeroVector;
		int32 ConnectionIndex = INDEX_NONE;
		FVector FlapPoint = FVector::ZeroVector;
		bool bHasFlapPoint = false;
		TArray<int32> AdditionalConnectionIndices;
		TArray<FVector> AdditionalFlapPoints;
	};
	struct FCenterSupportSample
	{
		FVector Location = FVector::ZeroVector;
		FVector Direction = FVector::ZeroVector;
	};

	TArray<FBoundarySample> BoundarySamples;
	TArray<FCenterSupportSample> CenterSupportSamples;
	for (int32 ConnectionIndex = 0;
		ConnectionIndex < Connections.Num();
		++ConnectionIndex)
	{
		const FProceduralRoadJunctionConnection& Connection =
			Connections[ConnectionIndex];
		if (!Connection.Road)
		{
			continue;
		}

		const FAppliedConnection* AppliedConnection = nullptr;
		for (const FAppliedConnection& CandidateAppliedConnection : AppliedConnections)
		{
			if (CandidateAppliedConnection.Road == Connection.Road
				&& CandidateAppliedConnection.Endpoint == Connection.Endpoint)
			{
				AppliedConnection = &CandidateAppliedConnection;
				break;
			}
		}
		if (!AppliedConnection)
		{
			continue;
		}

		FProceduralRoadJunctionEdgeGeometry EdgeGeometry;
		if (!Connection.Road->GetJunctionEdgeGeometry(
			Connection.Endpoint,
			AppliedConnection->TrimDistance,
			EdgeGeometry))
		{
			continue;
		}
		if (!EdgeGeometry.SurfacePoints.IsEmpty())
		{
			FCenterSupportSample& CenterSupportSample =
				CenterSupportSamples.AddDefaulted_GetRef();
			for (const FVector& SurfacePoint : EdgeGeometry.SurfacePoints)
			{
				CenterSupportSample.Location += SurfacePoint;
			}
			CenterSupportSample.Location /= EdgeGeometry.SurfacePoints.Num();
			CenterSupportSample.Direction = EdgeGeometry.Direction;
		}

		for (int32 EdgePointIndex = 0;
			EdgePointIndex < EdgeGeometry.SurfacePoints.Num();
			++EdgePointIndex)
		{
			FBoundarySample& BoundarySample = BoundarySamples.AddDefaulted_GetRef();
			BoundarySample.Location = EdgeGeometry.SurfacePoints[EdgePointIndex];
			BoundarySample.ConnectionIndex = ConnectionIndex;
			if (EdgeGeometry.bHasSideFlaps && EdgePointIndex == 0)
			{
				BoundarySample.FlapPoint = EdgeGeometry.LeftFlapPoint;
				BoundarySample.bHasFlapPoint = true;
			}
			else if (EdgeGeometry.bHasSideFlaps
				&& EdgePointIndex == EdgeGeometry.SurfacePoints.Num() - 1)
			{
				BoundarySample.FlapPoint = EdgeGeometry.RightFlapPoint;
				BoundarySample.bHasFlapPoint = true;
			}
		}
	}

	if (BoundarySamples.Num() < 3)
	{
		JunctionMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		return false;
	}

	FVector Center = ProjectToTerrain(GetActorLocation());
	if (!CenterSupportSamples.IsEmpty())
	{
		float WeightedHeight = 0.0f;
		float TotalWeight = 0.0f;
		float MinimumEdgeHeight = TNumericLimits<float>::Max();
		float MaximumEdgeHeight = -TNumericLimits<float>::Max();
		for (const FCenterSupportSample& CenterSupportSample : CenterSupportSamples)
		{
			const FVector2D HorizontalDirection(
				CenterSupportSample.Direction.X,
				CenterSupportSample.Direction.Y);
			const FVector2D ToCenter(
				Center.X - CenterSupportSample.Location.X,
				Center.Y - CenterSupportSample.Location.Y);
			const float HorizontalDirectionLength = HorizontalDirection.Size();
			float PredictedHeight = CenterSupportSample.Location.Z;
			if (HorizontalDirectionLength > KINDA_SMALL_NUMBER)
			{
				const float DistanceAlongApproach = FVector2D::DotProduct(
					ToCenter,
					HorizontalDirection / HorizontalDirectionLength);
				PredictedHeight += CenterSupportSample.Direction.Z
					/ HorizontalDirectionLength * DistanceAlongApproach;
			}
			const float Weight = 1.0f / FMath::Max(ToCenter.Size(), 1.0f);
			WeightedHeight += PredictedHeight * Weight;
			TotalWeight += Weight;
			MinimumEdgeHeight = FMath::Min(
				MinimumEdgeHeight,
				CenterSupportSample.Location.Z);
			MaximumEdgeHeight = FMath::Max(
				MaximumEdgeHeight,
				CenterSupportSample.Location.Z);
		}
		if (TotalWeight > KINDA_SMALL_NUMBER)
		{
			Center.Z = FMath::Clamp(
				WeightedHeight / TotalWeight,
				MinimumEdgeHeight - MaximumInteriorTerrainDeviation,
				MaximumEdgeHeight + MaximumInteriorTerrainDeviation);
		}
	}
	BoundarySamples.Sort(
		[Center](const FBoundarySample& First, const FBoundarySample& Second)
		{
			const float FirstAngle = FMath::Atan2(
				First.Location.Y - Center.Y,
				First.Location.X - Center.X);
			const float SecondAngle = FMath::Atan2(
				Second.Location.Y - Center.Y,
				Second.Location.X - Center.X);
			return !FMath::IsNearlyEqual(FirstAngle, SecondAngle)
				? FirstAngle < SecondAngle
				: FVector::DistSquared(First.Location, Center)
					< FVector::DistSquared(Second.Location, Center);
		});
	auto MergeFlapPoints = [](FBoundarySample& Target, const FBoundarySample& Source)
	{
		auto AddConnectionIndex = [&Target](int32 ConnectionIndex)
		{
			if (ConnectionIndex != INDEX_NONE
				&& ConnectionIndex != Target.ConnectionIndex)
			{
				Target.AdditionalConnectionIndices.AddUnique(ConnectionIndex);
			}
		};
		AddConnectionIndex(Source.ConnectionIndex);
		for (int32 ConnectionIndex : Source.AdditionalConnectionIndices)
		{
			AddConnectionIndex(ConnectionIndex);
		}
		if (!Source.bHasFlapPoint)
		{
			return;
		}
		if (!Target.bHasFlapPoint)
		{
			Target.FlapPoint = Source.FlapPoint;
			Target.bHasFlapPoint = true;
			return;
		}
		if (!Target.FlapPoint.Equals(Source.FlapPoint, 0.1f)
			&& !Target.AdditionalFlapPoints.ContainsByPredicate(
				[Source](const FVector& FlapPoint)
				{
					return FlapPoint.Equals(Source.FlapPoint, 0.1f);
				}))
		{
			Target.AdditionalFlapPoints.Add(Source.FlapPoint);
		}
	};
	auto FindSharedConnection = [](
		const FBoundarySample& First,
		const FBoundarySample& Second) -> int32
	{
		if (First.ConnectionIndex != INDEX_NONE
			&& (First.ConnectionIndex == Second.ConnectionIndex
				|| Second.AdditionalConnectionIndices.Contains(
					First.ConnectionIndex)))
		{
			return First.ConnectionIndex;
		}
		for (int32 ConnectionIndex : First.AdditionalConnectionIndices)
		{
			if (ConnectionIndex == Second.ConnectionIndex
				|| Second.AdditionalConnectionIndices.Contains(ConnectionIndex))
			{
				return ConnectionIndex;
			}
		}
		return INDEX_NONE;
	};
	for (int32 BoundaryIndex = BoundarySamples.Num() - 1;
		BoundaryIndex > 0;
		--BoundaryIndex)
	{
		if (BoundarySamples[BoundaryIndex].Location.Equals(
			BoundarySamples[BoundaryIndex - 1].Location,
			1.0f))
		{
			MergeFlapPoints(
				BoundarySamples[BoundaryIndex - 1],
				BoundarySamples[BoundaryIndex]);
			BoundarySamples.RemoveAt(BoundaryIndex);
		}
	}
	if (BoundarySamples.Num() > 2
		&& BoundarySamples[0].Location.Equals(
			BoundarySamples.Last().Location,
			1.0f))
	{
		MergeFlapPoints(BoundarySamples[0], BoundarySamples.Last());
		BoundarySamples.Pop();
	}
	if (BoundarySamples.Num() < 3)
	{
		JunctionMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		return false;
	}

	TArray<FBoundarySample> BoundaryPoints;
	const float SampleSpacing = FMath::Max(TerrainSampleSpacing, 25.0f);
	for (int32 BoundaryIndex = 0;
		BoundaryIndex < BoundarySamples.Num();
		++BoundaryIndex)
	{
		const FBoundarySample& Current = BoundarySamples[BoundaryIndex];
		const FBoundarySample& Next =
			BoundarySamples[(BoundaryIndex + 1) % BoundarySamples.Num()];
		BoundaryPoints.Add(Current);
		const int32 SharedConnectionIndex = FindSharedConnection(Current, Next);
		const float EdgeLength = FVector2D::Distance(
			FVector2D(Current.Location),
			FVector2D(Next.Location));
		const float EdgeSampleSpacing = SharedConnectionIndex != INDEX_NONE
			? SampleSpacing
			: FMath::Min(
				SampleSpacing,
				FMath::Max(GroundBlendSampleSpacing, 25.0f));
		const int32 EdgeSegmentCount = FMath::Max(
			1,
			FMath::CeilToInt(EdgeLength / EdgeSampleSpacing));
		for (int32 SegmentIndex = 1;
			SegmentIndex < EdgeSegmentCount;
			++SegmentIndex)
		{
			const float Alpha = static_cast<float>(SegmentIndex) / EdgeSegmentCount;
			const FVector DesiredPoint = FMath::Lerp(
				Current.Location,
				Next.Location,
				Alpha);
			FBoundarySample& BoundaryPoint = BoundaryPoints.AddDefaulted_GetRef();
			BoundaryPoint.Location = SharedConnectionIndex != INDEX_NONE
				? DesiredPoint
				: ProjectToTerrain(DesiredPoint);
			BoundaryPoint.ConnectionIndex = SharedConnectionIndex;
		}
	}

	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector2D> UVs;
	float MaximumRadius = 0.0f;
	for (const FBoundarySample& BoundaryPoint : BoundaryPoints)
	{
		MaximumRadius = FMath::Max(
			MaximumRadius,
			FVector2D::Distance(
				FVector2D(Center),
				FVector2D(BoundaryPoint.Location)));
	}
	const int32 RingCount = FMath::Max(
		1,
		FMath::CeilToInt(MaximumRadius / SampleSpacing));
	Vertices.Reserve(1 + RingCount * BoundaryPoints.Num());
	UVs.Reserve(Vertices.Max());

	Vertices.Add(GetActorTransform().InverseTransformPosition(Center));
	UVs.Add(FVector2D(
		Center.X / FMath::Max(UVWorldSize.X, 1.0f),
		Center.Y / FMath::Max(UVWorldSize.Y, 1.0f)));
	for (int32 RingIndex = 1; RingIndex <= RingCount; ++RingIndex)
	{
		const float RingAlpha = static_cast<float>(RingIndex) / RingCount;
		for (const FBoundarySample& BoundaryPoint : BoundaryPoints)
		{
			const FVector DesiredPoint = FMath::Lerp(
				Center,
				BoundaryPoint.Location,
				RingAlpha);
			FVector WorldPoint = BoundaryPoint.Location;
			if (RingIndex != RingCount)
			{
				WorldPoint = ProjectToTerrain(DesiredPoint);
				const float SupportedHeight = FMath::Lerp(
					Center.Z,
					BoundaryPoint.Location.Z,
					RingAlpha);
				const float TerrainInfluence =
					4.0f * RingAlpha * (1.0f - RingAlpha);
				const float AllowedDeviation =
					MaximumInteriorTerrainDeviation * TerrainInfluence;
				WorldPoint.Z = FMath::Clamp(
					WorldPoint.Z,
					SupportedHeight - AllowedDeviation,
					SupportedHeight + AllowedDeviation);
			}
			Vertices.Add(GetActorTransform().InverseTransformPosition(WorldPoint));
			UVs.Add(FVector2D(
				WorldPoint.X / FMath::Max(UVWorldSize.X, 1.0f),
				WorldPoint.Y / FMath::Max(UVWorldSize.Y, 1.0f)));
		}
	}

	for (int32 PointIndex = 0; PointIndex < BoundaryPoints.Num(); ++PointIndex)
	{
		Triangles.Add(0);
		Triangles.Add(PointIndex + 1);
		Triangles.Add(((PointIndex + 1) % BoundaryPoints.Num()) + 1);
	}
	for (int32 RingIndex = 1; RingIndex < RingCount; ++RingIndex)
	{
		const int32 InnerRingStart = 1 + (RingIndex - 1) * BoundaryPoints.Num();
		const int32 OuterRingStart = 1 + RingIndex * BoundaryPoints.Num();
		for (int32 PointIndex = 0; PointIndex < BoundaryPoints.Num(); ++PointIndex)
		{
			const int32 NextPointIndex = (PointIndex + 1) % BoundaryPoints.Num();
			Triangles.Add(InnerRingStart + PointIndex);
			Triangles.Add(OuterRingStart + PointIndex);
			Triangles.Add(OuterRingStart + NextPointIndex);
			Triangles.Add(InnerRingStart + PointIndex);
			Triangles.Add(OuterRingStart + NextPointIndex);
			Triangles.Add(InnerRingStart + NextPointIndex);
		}
	}

	auto OrientTrianglesUpward = [this](
		const TArray<FVector>& MeshVertices,
		TArray<int32>& MeshTriangles)
	{
		const FVector LocalWorldUp = GetActorTransform().InverseTransformVectorNoScale(
			FVector::UpVector);
		for (int32 TriangleIndex = 0;
			TriangleIndex + 2 < MeshTriangles.Num();
			TriangleIndex += 3)
		{
			const FVector& First = MeshVertices[MeshTriangles[TriangleIndex]];
			const FVector& Second = MeshVertices[MeshTriangles[TriangleIndex + 1]];
			const FVector& Third = MeshVertices[MeshTriangles[TriangleIndex + 2]];
			const FVector TriangleNormal = FVector::CrossProduct(
				Second - First,
				Third - First);
			if (FVector::DotProduct(TriangleNormal, LocalWorldUp) > 0.0f)
			{
				Swap(MeshTriangles[TriangleIndex + 1], MeshTriangles[TriangleIndex + 2]);
			}
		}
	};
	OrientTrianglesUpward(Vertices, Triangles);

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

	if (GroundBlendWidth > KINDA_SMALL_NUMBER)
	{
		TArray<AProceduralRoadJunctionActor*> NearbyJunctions;
		GatherNearbyJunctions(NearbyJunctions);
		TArray<FVector> BlendVertices;
		TArray<int32> BlendTriangles;
		TArray<FVector2D> BlendUVs;
		BlendVertices.Reserve(BoundaryPoints.Num() * 2);
		BlendUVs.Reserve(BoundaryPoints.Num() * 2);
		for (const FBoundarySample& BoundaryPoint : BoundaryPoints)
		{
			BlendVertices.Add(GetActorTransform().InverseTransformPosition(
				BoundaryPoint.Location));
			BlendUVs.Add(FVector2D(
				BoundaryPoint.Location.X / FMath::Max(UVWorldSize.X, 1.0f),
				BoundaryPoint.Location.Y / FMath::Max(UVWorldSize.Y, 1.0f)));
		}
		TArray<FVector> BlendOuterPoints;
		BlendOuterPoints.Reserve(BoundaryPoints.Num());
		for (const FBoundarySample& BoundaryPoint : BoundaryPoints)
		{
			FVector OuterPoint = BoundaryPoint.FlapPoint;
			if (!BoundaryPoint.bHasFlapPoint)
			{
				FVector OutwardDirection(
					BoundaryPoint.Location.X - Center.X,
					BoundaryPoint.Location.Y - Center.Y,
					0.0f);
				OutwardDirection.Normalize();
				const FVector DesiredOuterPoint =
					BoundaryPoint.Location + OutwardDirection * GroundBlendWidth;
				OuterPoint = ClampBlendPointToNearbyJunctions(
					BoundaryPoint.Location,
					DesiredOuterPoint,
					NearbyJunctions);
				OuterPoint = ProjectToTerrain(OuterPoint)
					- FVector::UpVector * GroundBlendEmbedDepth;
			}
			BlendOuterPoints.Add(OuterPoint);
		}

		for (int32 StartIndex = 0;
			StartIndex < BoundaryPoints.Num();
			++StartIndex)
		{
			if (!BoundaryPoints[StartIndex].bHasFlapPoint)
			{
				continue;
			}

			const int32 FirstNextIndex =
				(StartIndex + 1) % BoundaryPoints.Num();
			if (FindSharedConnection(
				BoundaryPoints[StartIndex],
				BoundaryPoints[FirstNextIndex]) != INDEX_NONE)
			{
				continue;
			}

			int32 EndIndex = FirstNextIndex;
			float SpanLength = FVector2D::Distance(
				FVector2D(BoundaryPoints[StartIndex].Location),
				FVector2D(BoundaryPoints[EndIndex].Location));
			int32 SearchCount = 1;
			while (!BoundaryPoints[EndIndex].bHasFlapPoint
				&& SearchCount < BoundaryPoints.Num())
			{
				const int32 NextIndex = (EndIndex + 1) % BoundaryPoints.Num();
				if (FindSharedConnection(
					BoundaryPoints[EndIndex],
					BoundaryPoints[NextIndex]) != INDEX_NONE)
				{
					break;
				}
				SpanLength += FVector2D::Distance(
					FVector2D(BoundaryPoints[EndIndex].Location),
					FVector2D(BoundaryPoints[NextIndex].Location));
				EndIndex = NextIndex;
				++SearchCount;
			}
			if (!BoundaryPoints[EndIndex].bHasFlapPoint
				|| SpanLength <= KINDA_SMALL_NUMBER)
			{
				continue;
			}

			const FVector StartOffset =
				BoundaryPoints[StartIndex].FlapPoint
				- BoundaryPoints[StartIndex].Location;
			const FVector EndOffset =
				BoundaryPoints[EndIndex].FlapPoint
				- BoundaryPoints[EndIndex].Location;
			float DistanceAlongSpan = 0.0f;
			int32 PointIndex = FirstNextIndex;
			int32 PreviousIndex = StartIndex;
			while (PointIndex != EndIndex)
			{
				DistanceAlongSpan += FVector2D::Distance(
					FVector2D(BoundaryPoints[PreviousIndex].Location),
					FVector2D(BoundaryPoints[PointIndex].Location));
				const float Alpha = FMath::Clamp(
					DistanceAlongSpan / SpanLength,
					0.0f,
					1.0f);
				const FVector DesiredOuterPoint =
					BoundaryPoints[PointIndex].Location
					+ FMath::Lerp(StartOffset, EndOffset, Alpha);
				FVector OuterPoint = ClampBlendPointToNearbyJunctions(
					BoundaryPoints[PointIndex].Location,
					DesiredOuterPoint,
					NearbyJunctions);
				BlendOuterPoints[PointIndex] = ProjectToTerrain(OuterPoint)
					- FVector::UpVector * GroundBlendEmbedDepth;
				PreviousIndex = PointIndex;
				PointIndex = (PointIndex + 1) % BoundaryPoints.Num();
			}
		}

		for (const FVector& OuterPoint : BlendOuterPoints)
		{
			BlendVertices.Add(GetActorTransform().InverseTransformPosition(OuterPoint));
			BlendUVs.Add(FVector2D(
				OuterPoint.X / FMath::Max(UVWorldSize.X, 1.0f),
				OuterPoint.Y / FMath::Max(UVWorldSize.Y, 1.0f)));
		}
		for (int32 PointIndex = 0; PointIndex < BoundaryPoints.Num(); ++PointIndex)
		{
			const int32 NextPointIndex =
				(PointIndex + 1) % BoundaryPoints.Num();
			const bool bRoadMouth = FindSharedConnection(
				BoundaryPoints[PointIndex],
				BoundaryPoints[NextPointIndex]) != INDEX_NONE;
			if (bRoadMouth)
			{
				continue;
			}
			const int32 OuterStart = BoundaryPoints.Num();
			if (!BlendVertices[OuterStart + PointIndex].Equals(
				BlendVertices[PointIndex],
				0.1f)
				|| !BlendVertices[OuterStart + NextPointIndex].Equals(
					BlendVertices[NextPointIndex],
					0.1f))
			{
				BlendTriangles.Add(PointIndex);
				BlendTriangles.Add(OuterStart + PointIndex);
				BlendTriangles.Add(OuterStart + NextPointIndex);
				BlendTriangles.Add(PointIndex);
				BlendTriangles.Add(OuterStart + NextPointIndex);
				BlendTriangles.Add(NextPointIndex);
			}
		}
		for (int32 PointIndex = 0; PointIndex < BoundaryPoints.Num(); ++PointIndex)
		{
			int32 PreviousFlapVertexIndex = BoundaryPoints.Num() + PointIndex;
			FVector PreviousFlapPoint = BoundaryPoints[PointIndex].FlapPoint;
			for (const FVector& AdditionalFlapPoint :
				BoundaryPoints[PointIndex].AdditionalFlapPoints)
			{
				const int32 SegmentCount = FMath::Max(
					1,
					FMath::CeilToInt(
						FVector2D::Distance(
							FVector2D(PreviousFlapPoint),
							FVector2D(AdditionalFlapPoint))
						/ FMath::Max(GroundBlendSampleSpacing, 25.0f)));
				for (int32 SegmentIndex = 1;
					SegmentIndex <= SegmentCount;
					++SegmentIndex)
				{
					const float Alpha =
						static_cast<float>(SegmentIndex) / SegmentCount;
					const FVector DesiredFlapPoint = FMath::Lerp(
						PreviousFlapPoint,
						AdditionalFlapPoint,
						Alpha);
					const FVector FlapPoint = SegmentIndex == SegmentCount
						? AdditionalFlapPoint
						: ProjectToTerrain(DesiredFlapPoint)
							- FVector::UpVector * GroundBlendEmbedDepth;
					const int32 FlapVertexIndex = BlendVertices.Add(
						GetActorTransform().InverseTransformPosition(FlapPoint));
					BlendUVs.Add(FVector2D(
						FlapPoint.X / FMath::Max(UVWorldSize.X, 1.0f),
						FlapPoint.Y / FMath::Max(UVWorldSize.Y, 1.0f)));
					BlendTriangles.Add(PointIndex);
					BlendTriangles.Add(PreviousFlapVertexIndex);
					BlendTriangles.Add(FlapVertexIndex);
					PreviousFlapVertexIndex = FlapVertexIndex;
				}
				PreviousFlapPoint = AdditionalFlapPoint;
			}
		}
		OrientTrianglesUpward(BlendVertices, BlendTriangles);
		TArray<FVector> BlendNormals;
		TArray<FProcMeshTangent> BlendTangents;
		UKismetProceduralMeshLibrary::CalculateTangentsForMesh(
			BlendVertices,
			BlendTriangles,
			BlendUVs,
			BlendNormals,
			BlendTangents);
		JunctionMesh->CreateMeshSection_LinearColor(
			1,
			BlendVertices,
			BlendTriangles,
			BlendNormals,
			BlendUVs,
			VertexColors,
			BlendTangents,
			false);
		JunctionMesh->SetMaterial(1, GetEffectiveGroundBlendMaterial());
	}

	if (bGenerateCollision)
	{
		TArray<FVector> ConvexVertices;
		const FVector LocalDown = GetActorTransform().InverseTransformVectorNoScale(
			-FVector::UpVector * CollisionThickness);
		const int32 BoundaryStart =
			1 + (RingCount - 1) * BoundaryPoints.Num();
		for (int32 PointIndex = 0; PointIndex < BoundaryPoints.Num(); ++PointIndex)
		{
			ConvexVertices.Add(Vertices[BoundaryStart + PointIndex]);
			ConvexVertices.Add(Vertices[BoundaryStart + PointIndex] + LocalDown);
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
	TArray<FHitResult> Hits;
	GetWorld()->LineTraceMultiByChannel(
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
		RebuildJunctionInternal(false);
		QueueNearbyJunctionRebuilds();
		return;
	}

	QueueEditorRebuild();
}

void AProceduralRoadJunctionActor::PostEditChangeProperty(
	FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	QueueEditorRebuild();
	QueueNearbyJunctionRebuilds();
}

void AProceduralRoadJunctionActor::PostEditUndo()
{
	Super::PostEditUndo();
	QueueEditorRebuild();
	QueueNearbyJunctionRebuilds();
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
					RebuildJunctionInternal(false);
				}
				return false;
			}),
		0.15f);
}

void AProceduralRoadJunctionActor::QueueNearbyJunctionRebuilds()
{
	TArray<AProceduralRoadJunctionActor*> NearbyJunctions;
	GatherNearbyJunctions(NearbyJunctions);
	for (AProceduralRoadJunctionActor* NearbyJunction : NearbyJunctions)
	{
		NearbyJunction->QueueEditorRebuild();
	}
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
