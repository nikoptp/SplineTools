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
		return GetRequestedTrimDistance(Connection);
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
				PairedTrimDistance = JunctionIterator->GetRequestedTrimDistance(OtherConnection);
				bHasPairedJunction = true;
				break;
			}
		}
		if (bHasPairedJunction)
		{
			break;
		}
	}

	const float RequestedTrimDistance = GetRequestedTrimDistance(Connection);
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

float AProceduralRoadJunctionActor::GetRequestedTrimDistance(
	const FProceduralRoadJunctionConnection& Connection) const
{
	return FMath::Max(Connection.TrimDistance, 0.0f)
		+ FMath::Max(RoadMouthPadding, 0.0f);
}

void AProceduralRoadJunctionActor::GatherNearbyJunctions(
	const FVector& JunctionCenter,
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
				FVector2D(JunctionCenter),
				FVector2D(Junction->GetActorLocation())) <= SearchRadiusSquared)
		{
			OutJunctions.Add(Junction);
		}
	}
}

FVector AProceduralRoadJunctionActor::ClampBlendPointToNearbyJunctions(
	const FVector& JunctionCenter,
	const FVector& InnerPoint,
	const FVector& DesiredOuterPoint,
	const TArray<AProceduralRoadJunctionActor*>& NearbyJunctions) const
{
	const FVector2D SelfCenter(JunctionCenter);
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
		TMap<int32, FProceduralRoadLineEdge> LineEdges;
	};
	struct FCenterSupportSample
	{
		FVector Location = FVector::ZeroVector;
		FVector Direction = FVector::ZeroVector;
	};
	struct FJunctionPortal
	{
		int32 ConnectionIndex = INDEX_NONE;
		TObjectPtr<AProceduralRoadActor> Road;
		ERoadSplineEndpoint Endpoint = ERoadSplineEndpoint::End;
		float TrimDistance = 0.0f;
		TArray<FVector> SurfacePoints;
		FVector Direction = FVector::ZeroVector;
		FVector LeftFlapPoint = FVector::ZeroVector;
		FVector RightFlapPoint = FVector::ZeroVector;
		bool bHasSideFlaps = false;
	};

	// Keep each cached road mouth together. Sorting individual cross-section
	// vertices lets adjacent roads interleave and produces crossing boundary
	// spans at skewed intersections.
	TArray<FJunctionPortal> Portals;
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
		if (EdgeGeometry.SurfacePoints.Num() < 2)
		{
			continue;
		}

		FCenterSupportSample& CenterSupportSample =
			CenterSupportSamples.AddDefaulted_GetRef();
		for (const FVector& SurfacePoint : EdgeGeometry.SurfacePoints)
		{
			CenterSupportSample.Location += SurfacePoint;
		}
		CenterSupportSample.Location /= EdgeGeometry.SurfacePoints.Num();
		CenterSupportSample.Direction = EdgeGeometry.Direction;

		FJunctionPortal& Portal = Portals.AddDefaulted_GetRef();
		Portal.ConnectionIndex = ConnectionIndex;
		Portal.Road = Connection.Road;
		Portal.Endpoint = Connection.Endpoint;
		Portal.TrimDistance = AppliedConnection->TrimDistance;
		Portal.SurfacePoints = MoveTemp(EdgeGeometry.SurfacePoints);
		Portal.Direction = EdgeGeometry.Direction;
		Portal.LeftFlapPoint = EdgeGeometry.LeftFlapPoint;
		Portal.RightFlapPoint = EdgeGeometry.RightFlapPoint;
		Portal.bHasSideFlaps = EdgeGeometry.bHasSideFlaps;
	}

	if (Portals.Num() < 2)
	{
		JunctionMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		return false;
	}

	FVector CenterCandidate = GetActorLocation();
	float XX = 0.0f;
	float XY = 0.0f;
	float YY = 0.0f;
	float BX = 0.0f;
	float BY = 0.0f;
	int32 ValidCenterDirections = 0;
	for (const FJunctionPortal& Portal : Portals)
	{
		FVector PortalCenter = FVector::ZeroVector;
		for (const FVector& Point : Portal.SurfacePoints)
		{
			PortalCenter += Point;
		}
		PortalCenter /= Portal.SurfacePoints.Num();
		const FVector2D Direction(
			Portal.Direction.X,
			Portal.Direction.Y);
		if (Direction.SizeSquared() <= KINDA_SMALL_NUMBER)
		{
			continue;
		}
		const FVector2D Normal(-Direction.Y, Direction.X);
		const float Projection = FVector2D::DotProduct(
			Normal,
			FVector2D(PortalCenter));
		XX += Normal.X * Normal.X;
		XY += Normal.X * Normal.Y;
		YY += Normal.Y * Normal.Y;
		BX += Normal.X * Projection;
		BY += Normal.Y * Projection;
		++ValidCenterDirections;
	}
	const float CenterDeterminant = XX * YY - XY * XY;
	if (ValidCenterDirections >= 2
		&& FMath::Abs(CenterDeterminant) > KINDA_SMALL_NUMBER)
	{
		CenterCandidate.X = (BX * YY - BY * XY) / CenterDeterminant;
		CenterCandidate.Y = (XX * BY - XY * BX) / CenterDeterminant;
	}

	FVector Center = CenterCandidate;
	float CenterTerrainHeight = -TNumericLimits<float>::Max();
	FHitResult CenterTerrainHit;
	const bool bHasCenterTerrainHit = bAlignToTerrain
		&& TraceTerrain(CenterCandidate, CenterTerrainHit);
	if (bHasCenterTerrainHit)
	{
		const FVector CenterTerrainPoint = CenterTerrainHit.ImpactPoint
			+ CenterTerrainHit.ImpactNormal.GetSafeNormal() * SurfaceOffset;
		Center = CenterTerrainPoint;
		CenterTerrainHeight = CenterTerrainPoint.Z;
	}
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
	// Fit the center to nearby ground without letting an isolated depression pull it below its roads.
	if (bAlignToTerrain && InteriorSmoothingStrength > 0.0f)
	{
		float GroundHeightSum = 0.0f;
		int32 GroundHitCount = 0;
		for (int32 Index = 0; Index < 9; ++Index)
		{
			const float Angle = (Index - 1) * PI / 4.0f;
			const FVector Offset = Index == 0 ? FVector::ZeroVector : FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f) * FMath::Max(TerrainSampleSpacing * 2.0f, 150.0f);
			FHitResult Hit;
			if (TraceTerrain(Center + Offset, Hit) && FMath::Abs(Hit.ImpactPoint.Z + SurfaceOffset - Center.Z) <= MaximumInteriorTerrainDeviation)
			{
				GroundHeightSum += Hit.ImpactPoint.Z + SurfaceOffset;
				++GroundHitCount;
			}
		}
		// A small isolated collider is not enough evidence to move the shared road-supported center.
		if (GroundHitCount >= 5)
		{
			Center.Z = FMath::Lerp(Center.Z, GroundHeightSum / GroundHitCount, FMath::Clamp(InteriorSmoothingStrength, 0.0f, 1.0f));
		}
	}
	if (bHasCenterTerrainHit)
	{
		// Road-slope extrapolation can undercut the landscape at the convergence
		// point on a shallow grade; never let the center sink below its terrain sample.
		Center.Z = FMath::Max(Center.Z, CenterTerrainHeight);
	}
	TArray<FBoundarySample> BoundarySamples;
	Portals.Sort(
		[Center](const FJunctionPortal& First, const FJunctionPortal& Second)
		{
			FVector FirstCenter = FVector::ZeroVector;
			for (const FVector& Point : First.SurfacePoints)
			{
				FirstCenter += Point;
			}
			FirstCenter /= First.SurfacePoints.Num();
			FVector SecondCenter = FVector::ZeroVector;
			for (const FVector& Point : Second.SurfacePoints)
			{
				SecondCenter += Point;
			}
			SecondCenter /= Second.SurfacePoints.Num();
			const float FirstAngle = FMath::Atan2(
				FirstCenter.Y - Center.Y,
				FirstCenter.X - Center.X);
			const float SecondAngle = FMath::Atan2(
				SecondCenter.Y - Center.Y,
				SecondCenter.X - Center.X);
			return !FMath::IsNearlyEqual(FirstAngle, SecondAngle)
				? FirstAngle < SecondAngle
				: First.ConnectionIndex < Second.ConnectionIndex;
		});
	for (const FJunctionPortal& Portal : Portals)
	{
		// Orient each portal along the counter-clockwise perimeter. The road
		// surface vertices remain in their authored order inside the portal;
		// only the portal as a whole may be reversed.
		FVector PortalCenter = FVector::ZeroVector;
		for (const FVector& Point : Portal.SurfacePoints)
		{
			PortalCenter += Point;
		}
		PortalCenter /= Portal.SurfacePoints.Num();
		const FVector2D Radial(
			PortalCenter.X - Center.X,
			PortalCenter.Y - Center.Y);
		const FVector2D PortalRadial = Radial.SizeSquared() > KINDA_SMALL_NUMBER
			? Radial.GetSafeNormal()
			: FVector2D(Portal.Direction.X, Portal.Direction.Y).GetSafeNormal();
		const FVector2D CounterClockwiseTangent(
			-PortalRadial.Y,
			PortalRadial.X);
		const FVector2D PortalSpan(
			Portal.SurfacePoints.Last().X - Portal.SurfacePoints[0].X,
			Portal.SurfacePoints.Last().Y - Portal.SurfacePoints[0].Y);
		const bool bReversePortal = FVector2D::DotProduct(
			PortalSpan,
			CounterClockwiseTangent) < 0.0f;
		for (int32 PortalPointIndex = 0;
			PortalPointIndex < Portal.SurfacePoints.Num();
			++PortalPointIndex)
		{
			const int32 OriginalPointIndex = bReversePortal
				? Portal.SurfacePoints.Num() - 1 - PortalPointIndex
				: PortalPointIndex;
			FBoundarySample& BoundarySample = BoundarySamples.AddDefaulted_GetRef();
			BoundarySample.Location = Portal.SurfacePoints[OriginalPointIndex];
			BoundarySample.ConnectionIndex = Portal.ConnectionIndex;
			if (PortalPointIndex == 0 || PortalPointIndex == Portal.SurfacePoints.Num() - 1)
			{
				const bool bOriginalLeft = OriginalPointIndex == 0;
				FProceduralRoadLineEdge LineEdge;
				if (Portal.Road->GetJunctionSideLineEdge(
					Portal.Endpoint,
					Portal.TrimDistance,
					bOriginalLeft,
					LineEdge))
				{
					BoundarySample.LineEdges.Add(Portal.ConnectionIndex, LineEdge);
				}
			}
			if (Portal.bHasSideFlaps && OriginalPointIndex == 0)
			{
				BoundarySample.FlapPoint = Portal.LeftFlapPoint;
				BoundarySample.bHasFlapPoint = true;
			}
			else if (Portal.bHasSideFlaps
				&& OriginalPointIndex == Portal.SurfacePoints.Num() - 1)
			{
				BoundarySample.FlapPoint = Portal.RightFlapPoint;
				BoundarySample.bHasFlapPoint = true;
			}
		}
	}

	auto MergeFlapPoints = [](FBoundarySample& Target, const FBoundarySample& Source)
	{
		Target.LineEdges.Append(Source.LineEdges);
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
	TArray<float> SupportedHeights;
	TArray<float> AllowedDeviations;
	float MaximumRadius = 0.0f;
	for (const FBoundarySample& BoundaryPoint : BoundaryPoints)
	{
		MaximumRadius = FMath::Max(
			MaximumRadius,
			FVector2D::Distance(
				FVector2D(Center),
				FVector2D(BoundaryPoint.Location)));
	}
	const int32 BoundaryPointCount = BoundaryPoints.Num();
	const int32 InteriorPointCount = FMath::Min(BoundaryPointCount, 24);
	const float InteriorRingSpacing = FMath::Max(SampleSpacing * 4.0f, 300.0f);
	const int32 RingCount = FMath::Clamp(
		FMath::CeilToInt(MaximumRadius / InteriorRingSpacing),
		2,
		4);
	const int32 BoundaryStart =
		1 + (RingCount - 1) * InteriorPointCount;
	TArray<float> BoundaryDistances;
	BoundaryDistances.SetNum(BoundaryPointCount + 1);
	float BoundaryPerimeter = 0.0f;
	for (int32 BoundaryIndex = 0;
		BoundaryIndex < BoundaryPointCount;
		++BoundaryIndex)
	{
		BoundaryDistances[BoundaryIndex] = BoundaryPerimeter;
		BoundaryPerimeter += FVector2D::Distance(
			FVector2D(BoundaryPoints[BoundaryIndex].Location),
			FVector2D(BoundaryPoints[(BoundaryIndex + 1) % BoundaryPointCount].Location));
	}
	BoundaryDistances[BoundaryPointCount] = BoundaryPerimeter;
	Vertices.Reserve(BoundaryStart + BoundaryPointCount);
	UVs.Reserve(Vertices.Max());

	Vertices.Add(GetActorTransform().InverseTransformPosition(Center));
	SupportedHeights.Add(Center.Z);
	AllowedDeviations.Add(0.0f);
	UVs.Add(FVector2D(
		Center.X / FMath::Max(UVWorldSize.X, 1.0f),
		Center.Y / FMath::Max(UVWorldSize.Y, 1.0f)));
	for (int32 RingIndex = 1; RingIndex < RingCount; ++RingIndex)
	{
		const float RingAlpha = static_cast<float>(RingIndex) / RingCount;
		for (int32 InteriorPointIndex = 0;
			InteriorPointIndex < InteriorPointCount;
			++InteriorPointIndex)
		{
			const float BoundaryDistance = BoundaryPerimeter
				* static_cast<float>(InteriorPointIndex) / InteriorPointCount;
			int32 BoundaryIndex = 0;
			while (BoundaryIndex + 1 < BoundaryPointCount
				&& BoundaryDistances[BoundaryIndex + 1] < BoundaryDistance)
			{
				++BoundaryIndex;
			}
			const int32 NextBoundaryIndex =
				(BoundaryIndex + 1) % BoundaryPointCount;
			const FBoundarySample& BoundaryPoint = BoundaryPoints[BoundaryIndex];
			const FBoundarySample& NextBoundaryPoint =
				BoundaryPoints[NextBoundaryIndex];
			const float BoundarySegmentLength =
				BoundaryDistances[BoundaryIndex + 1] - BoundaryDistances[BoundaryIndex];
			const float BoundaryAlpha = BoundarySegmentLength > KINDA_SMALL_NUMBER
				? (BoundaryDistance - BoundaryDistances[BoundaryIndex]) / BoundarySegmentLength
				: 0.0f;
			const FVector BoundaryLocation = FMath::Lerp(
				BoundaryPoint.Location,
				NextBoundaryPoint.Location,
				BoundaryAlpha);
			const FVector DesiredPoint = FMath::Lerp(
				Center,
				BoundaryLocation,
				RingAlpha);
			FVector WorldPoint = DesiredPoint;
			const float SupportedHeight = FMath::Lerp(
				Center.Z,
				BoundaryLocation.Z,
				RingAlpha);
			const float TerrainInfluence =
				4.0f * RingAlpha * (1.0f - RingAlpha);
			const float AllowedTerrainRaise =
				MaximumInteriorTerrainDeviation * TerrainInfluence;
			// The terrain sample may lift the patch toward a ridge, but an
			// isolated depression must not pull it below the edge-supported
			// surface and make the junction disappear into the ground.
			const FVector TerrainPoint = ProjectToTerrain(DesiredPoint);
			WorldPoint.Z = FMath::Clamp(
				TerrainPoint.Z,
				SupportedHeight,
				SupportedHeight + AllowedTerrainRaise);
			Vertices.Add(GetActorTransform().InverseTransformPosition(WorldPoint));
			SupportedHeights.Add(DesiredPoint.Z);
			AllowedDeviations.Add(AllowedTerrainRaise);
			UVs.Add(FVector2D(
				WorldPoint.X / FMath::Max(UVWorldSize.X, 1.0f),
				WorldPoint.Y / FMath::Max(UVWorldSize.Y, 1.0f)));
		}
	}
	for (const FBoundarySample& BoundaryPoint : BoundaryPoints)
	{
		Vertices.Add(GetActorTransform().InverseTransformPosition(BoundaryPoint.Location));
		SupportedHeights.Add(BoundaryPoint.Location.Z);
		AllowedDeviations.Add(0.0f);
		UVs.Add(FVector2D(
			BoundaryPoint.Location.X / FMath::Max(UVWorldSize.X, 1.0f),
			BoundaryPoint.Location.Y / FMath::Max(UVWorldSize.Y, 1.0f)));
	}

	for (int32 PointIndex = 0; PointIndex < InteriorPointCount; ++PointIndex)
	{
		Triangles.Add(0);
		Triangles.Add(PointIndex + 1);
		Triangles.Add(((PointIndex + 1) % InteriorPointCount) + 1);
	}
	for (int32 RingIndex = 1; RingIndex < RingCount - 1; ++RingIndex)
	{
		const int32 InnerRingStart = 1 + (RingIndex - 1) * InteriorPointCount;
		const int32 OuterRingStart = 1 + RingIndex * InteriorPointCount;
		for (int32 PointIndex = 0; PointIndex < InteriorPointCount; ++PointIndex)
		{
			const int32 NextPointIndex = (PointIndex + 1) % InteriorPointCount;
			Triangles.Add(InnerRingStart + PointIndex);
			Triangles.Add(OuterRingStart + PointIndex);
			Triangles.Add(OuterRingStart + NextPointIndex);
			Triangles.Add(InnerRingStart + PointIndex);
			Triangles.Add(OuterRingStart + NextPointIndex);
			Triangles.Add(InnerRingStart + NextPointIndex);
		}
	}
	int32 InnerPointIndex = 0;
	int32 OuterPointIndex = 0;
	while (InnerPointIndex < InteriorPointCount
		|| OuterPointIndex < BoundaryPointCount)
	{
		const int32 NextInnerPointIndex =
			(InnerPointIndex + 1) % InteriorPointCount;
		const int32 NextOuterPointIndex =
			(OuterPointIndex + 1) % BoundaryPointCount;
		const float NextInnerProgress =
			InnerPointIndex < InteriorPointCount
				? static_cast<float>(InnerPointIndex + 1) / InteriorPointCount
				: TNumericLimits<float>::Max();
		const float NextOuterProgress =
			OuterPointIndex < BoundaryPointCount
				? BoundaryDistances[OuterPointIndex + 1] / BoundaryPerimeter
				: TNumericLimits<float>::Max();
		const int32 InnerVertex =
			1 + (RingCount - 2) * InteriorPointCount
			+ InnerPointIndex % InteriorPointCount;
		const int32 OuterVertex =
			BoundaryStart + OuterPointIndex % BoundaryPointCount;
		if (InnerPointIndex < InteriorPointCount
			&& OuterPointIndex < BoundaryPointCount
			&& FMath::IsNearlyEqual(NextInnerProgress, NextOuterProgress))
		{
			Triangles.Add(InnerVertex);
			Triangles.Add(OuterVertex);
			Triangles.Add(BoundaryStart + NextOuterPointIndex);
			Triangles.Add(InnerVertex);
			Triangles.Add(BoundaryStart + NextOuterPointIndex);
			Triangles.Add(1 + (RingCount - 2) * InteriorPointCount + NextInnerPointIndex);
			++InnerPointIndex;
			++OuterPointIndex;
		}
		else if (NextInnerProgress < NextOuterProgress)
		{
			Triangles.Add(InnerVertex);
			Triangles.Add(OuterVertex);
			Triangles.Add(1 + (RingCount - 2) * InteriorPointCount + NextInnerPointIndex);
			++InnerPointIndex;
		}
		else
		{
			Triangles.Add(InnerVertex);
			Triangles.Add(OuterVertex);
			Triangles.Add(BoundaryStart + NextOuterPointIndex);
			++OuterPointIndex;
		}
	}

	// Smooth deviations from the supported surface, preserving planar slopes and all seam vertices.
	if (InteriorSmoothingIterations > 0 && InteriorSmoothingStrength > 0.0f)
	{
		TArray<TArray<int32>> Neighbors;
		Neighbors.SetNum(Vertices.Num());
		for (int32 Index = 0; Index < Triangles.Num(); Index += 3)
		{
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				Neighbors[Triangles[Index + Corner]].AddUnique(Triangles[Index + (Corner + 1) % 3]);
				Neighbors[Triangles[Index + Corner]].AddUnique(Triangles[Index + (Corner + 2) % 3]);
			}
		}
		TArray<float> Residuals;
		for (int32 Index = 0; Index < Vertices.Num(); ++Index)
		{
			Residuals.Add(GetActorTransform().TransformPosition(Vertices[Index]).Z - SupportedHeights[Index]);
		}
		for (int32 Iteration = 0; Iteration < FMath::Clamp(InteriorSmoothingIterations, 0, 16); ++Iteration)
		{
			TArray<float> Smoothed = Residuals;
			for (int32 Index = 1; Index < BoundaryStart; ++Index)
			{
				float WeightedResidual = 0.0f;
				float TotalWeight = 0.0f;
				for (int32 Neighbor : Neighbors[Index])
				{
					const float Weight = 1.0f / FMath::Max(FVector::DistSquared(Vertices[Index], Vertices[Neighbor]), 1.0);
					WeightedResidual += Residuals[Neighbor] * Weight;
					TotalWeight += Weight;
				}
				if (TotalWeight > 0.0f)
				{
					Smoothed[Index] = FMath::Clamp(FMath::Lerp(Residuals[Index], WeightedResidual / TotalWeight, FMath::Clamp(InteriorSmoothingStrength, 0.0f, 1.0f)), -AllowedDeviations[Index], AllowedDeviations[Index]);
				}
			}
			Residuals = MoveTemp(Smoothed);
		}
		for (int32 Index = 1; Index < BoundaryStart; ++Index)
		{
			FVector WorldPoint = GetActorTransform().TransformPosition(Vertices[Index]);
			WorldPoint.Z = SupportedHeights[Index] + Residuals[Index];
			Vertices[Index] = GetActorTransform().InverseTransformPosition(WorldPoint);
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
		GatherNearbyJunctions(Center, NearbyJunctions);
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
					Center,
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
					Center,
					BoundaryPoints[PointIndex].Location,
					DesiredOuterPoint,
					NearbyJunctions);
				BlendOuterPoints[PointIndex] = ProjectToTerrain(OuterPoint)
					- FVector::UpVector * GroundBlendEmbedDepth;
				PreviousIndex = PointIndex;
				PointIndex = (PointIndex + 1) % BoundaryPoints.Num();
			}
		}

		for (int32 PointIndex = 0;
			PointIndex < BlendOuterPoints.Num();
			++PointIndex)
		{
			const FVector& OuterPoint = BlendOuterPoints[PointIndex];
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

	if (bGenerateEdgeLines)
	{
		auto FindLineEdge = [](const FBoundarySample& Sample, const FVector& TowardSpan) -> const FProceduralRoadLineEdge*
		{
			const FProceduralRoadLineEdge* Best = nullptr;
			double BestAlignment = -TNumericLimits<double>::Max();
			for (const TPair<int32, FProceduralRoadLineEdge>& Candidate : Sample.LineEdges)
			{
				const double Alignment = FVector::DotProduct(Candidate.Value.Direction, TowardSpan.GetSafeNormal());
				if (Alignment > BestAlignment)
				{
					BestAlignment = Alignment;
					Best = &Candidate.Value;
				}
			}
			return Best;
		};
		int32 LineSectionIndex = 2;
		for (int32 Index = 0; Index < BoundarySamples.Num(); ++Index)
		{
			const int32 Next = (Index + 1) % BoundarySamples.Num();
			if (FindSharedConnection(BoundarySamples[Index], BoundarySamples[Next]) != INDEX_NONE)
			{
				continue;
			}
			const FProceduralRoadLineEdge* Start = FindLineEdge(BoundarySamples[Index], BoundarySamples[Next].Location - BoundarySamples[Index].Location);
			const FProceduralRoadLineEdge* End = FindLineEdge(BoundarySamples[Next], BoundarySamples[Index].Location - BoundarySamples[Next].Location);
			if (Start && End)
			{
				GenerateCornerLine(*Start, *End, Vertices, Triangles, LineSectionIndex);
			}
		}
	}

	if (bGenerateCollision)
	{
		TArray<FVector> ConvexVertices;
		const FVector LocalDown = GetActorTransform().InverseTransformVectorNoScale(
			-FVector::UpVector * CollisionThickness);
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

void AProceduralRoadJunctionActor::GenerateCornerLine(const FProceduralRoadLineEdge& Start, const FProceduralRoadLineEdge& End, const TArray<FVector>& SurfaceVertices, const TArray<int32>& SurfaceTriangles, int32& SectionIndex)
{
	const FVector StartCenter = (Start.Inner + Start.Outer) * 0.5f;
	const FVector EndCenter = (End.Inner + End.Outer) * 0.5f;
	const float SpanLength = FVector::Dist2D(StartCenter, EndCenter);
	if (SpanLength < 1.0f)
	{
		return;
	}
	TArray<FVector> WorldSurface;
	WorldSurface.Reserve(SurfaceVertices.Num());
	for (const FVector& Vertex : SurfaceVertices)
	{
		WorldSurface.Add(GetActorTransform().TransformPosition(Vertex));
	}
	auto ProjectToPatch = [&](FVector& Point, float Offset) -> bool
	{
		bool bFoundSurface = false;
		float BestHeightError = TNumericLimits<float>::Max();
		float BestSurfaceZ = 0.0f;
		// A coarse radial patch can have overlapping XY triangles at a
		// concave edge. Select the triangle closest to the unprojected road
		// line height instead of depending on triangle insertion order.
		for (int32 Index = 0; Index < SurfaceTriangles.Num(); Index += 3)
		{
			const FVector AB = WorldSurface[SurfaceTriangles[Index + 1]] - WorldSurface[SurfaceTriangles[Index]];
			const FVector AC = WorldSurface[SurfaceTriangles[Index + 2]] - WorldSurface[SurfaceTriangles[Index]];
			const FVector AP = Point - WorldSurface[SurfaceTriangles[Index]];
			const double Denominator = AB.X * AC.Y - AB.Y * AC.X;
			if (FMath::Abs(Denominator) <= UE_DOUBLE_SMALL_NUMBER)
			{
				continue;
			}
			const double U = (AP.X * AC.Y - AP.Y * AC.X) / Denominator;
			const double V = (AB.X * AP.Y - AB.Y * AP.X) / Denominator;
			if (U >= -0.0001 && V >= -0.0001 && U + V <= 1.0001)
			{
				const float SurfaceZ = WorldSurface[SurfaceTriangles[Index]].Z
					+ U * AB.Z
					+ V * AC.Z;
				const float HeightError = FMath::Abs(SurfaceZ - Point.Z);
				if (!bFoundSurface || HeightError < BestHeightError)
				{
					bFoundSurface = true;
					BestHeightError = HeightError;
					BestSurfaceZ = SurfaceZ;
				}
			}
		}
		if (!bFoundSurface)
		{
			return false;
		}
		Point.Z = BestSurfaceZ + Offset;
		return true;
	};

	const int32 SegmentCount = FMath::Max(2, FMath::CeilToInt(SpanLength * 1.7f / FMath::Max(EdgeLineSampleSpacing, 10.0f)) / 2 * 2);
	TArray<FVector> InnerPoints;
	TArray<FVector> OuterPoints;
	bool bValidCurve = false;
	// Shorten handles if an unusually tight junction cannot contain the full rounded corner.
	for (float HandleScale : {0.35f, 0.175f, 0.0f})
	{
		InnerPoints.Reset();
		OuterPoints.Reset();
		const FVector ControlStart = StartCenter + Start.Direction.GetSafeNormal2D() * SpanLength * HandleScale;
		const FVector ControlEnd = EndCenter + End.Direction.GetSafeNormal2D() * SpanLength * HandleScale;
		bValidCurve = true;
		for (int32 Index = 0; Index <= SegmentCount; ++Index)
		{
			if (Index == 0 || Index == SegmentCount)
			{
				InnerPoints.Add(Index == 0 ? Start.Inner : End.Inner);
				OuterPoints.Add(Index == 0 ? Start.Outer : End.Outer);
				continue;
			}
			const float Alpha = static_cast<float>(Index) / SegmentCount;
			const float Remaining = 1.0f - Alpha;
			const FVector Position = StartCenter * Remaining * Remaining * Remaining + ControlStart * 3.0f * Remaining * Remaining * Alpha + ControlEnd * 3.0f * Remaining * Alpha * Alpha + EndCenter * Alpha * Alpha * Alpha;
			const FVector Tangent = ((ControlStart - StartCenter) * Remaining * Remaining + (ControlEnd - ControlStart) * 2.0f * Remaining * Alpha + (EndCenter - ControlEnd) * Alpha * Alpha).GetSafeNormal2D();
			const float InwardSign = FVector::DotProduct(FVector::CrossProduct(FVector::UpVector, Start.Direction), Start.Inner - Start.Outer) >= 0.0 ? 1.0f : -1.0f;
			const FVector Inward = FVector::CrossProduct(FVector::UpVector, Tangent) * InwardSign;
			const float HalfWidth = FMath::Lerp(FVector::Dist2D(Start.Inner, Start.Outer), FVector::Dist2D(End.Inner, End.Outer), Alpha) * 0.5f;
			FVector Inner = Position + Inward * HalfWidth;
			FVector Outer = Position - Inward * HalfWidth;
			const float Offset = FMath::Lerp(Start.SurfaceOffset, End.SurfaceOffset, Alpha);
			if (!ProjectToPatch(Inner, Offset) || !ProjectToPatch(Outer, Offset))
			{
				bValidCurve = false;
				break;
			}
			InnerPoints.Add(Inner);
			OuterPoints.Add(Outer);
		}
		if (bValidCurve)
		{
			break;
		}
	}
	if (!bValidCurve)
	{
		return;
	}
	TArray<float> Distances;
	Distances.Add(0.0f);
	for (int32 Index = 1; Index <= SegmentCount; ++Index)
	{
		Distances.Add(Distances.Last() + FVector::Distance((InnerPoints[Index] + OuterPoints[Index]) * 0.5f, (InnerPoints[Index - 1] + OuterPoints[Index - 1]) * 0.5f));
	}
	auto AddHalf = [&](const FProceduralRoadLineEdge& Edge, int32 First, int32 Last, bool bReverse)
	{
		TArray<FVector> LineVertices;
		TArray<int32> LineTriangles;
		TArray<FVector2D> LineUVs;
		for (int32 Index = First; Index <= Last; ++Index)
		{
			const float V = Edge.V + (bReverse ? Distances.Last() - Distances[Index] : Distances[Index]) * Edge.VDirection / FMath::Max(Edge.UVWorldLength, 1.0f);
			LineVertices.Add(GetActorTransform().InverseTransformPosition(InnerPoints[Index]));
			LineVertices.Add(GetActorTransform().InverseTransformPosition(OuterPoints[Index]));
			LineUVs.Add(FVector2D(1.0f - Edge.OuterU, V));
			LineUVs.Add(FVector2D(Edge.OuterU, V));
			if (Index > First)
			{
				const int32 Current = (Index - First) * 2;
				LineTriangles.Append({Current - 2, Current, Current - 1, Current - 1, Current, Current + 1});
			}
		}
		const FVector LocalUp = GetActorTransform().InverseTransformVectorNoScale(FVector::UpVector);
		for (int32 Index = 0; Index < LineTriangles.Num(); Index += 3)
		{
			if (FVector::DotProduct(FVector::CrossProduct(LineVertices[LineTriangles[Index + 1]] - LineVertices[LineTriangles[Index]], LineVertices[LineTriangles[Index + 2]] - LineVertices[LineTriangles[Index]]), LocalUp) > 0.0)
			{
				Swap(LineTriangles[Index + 1], LineTriangles[Index + 2]);
			}
		}
		TArray<FVector> Normals;
		TArray<FProcMeshTangent> Tangents;
		UKismetProceduralMeshLibrary::CalculateTangentsForMesh(LineVertices, LineTriangles, LineUVs, Normals, Tangents);
		JunctionMesh->CreateMeshSection_LinearColor(SectionIndex, LineVertices, LineTriangles, Normals, LineUVs, TArray<FLinearColor>(), Tangents, false);
		JunctionMesh->SetMaterial(SectionIndex++, Edge.Material);
	};
	AddHalf(Start, 0, SegmentCount / 2, false);
	AddHalf(End, SegmentCount / 2, SegmentCount, true);
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
	GatherNearbyJunctions(GetActorLocation(), NearbyJunctions);
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
