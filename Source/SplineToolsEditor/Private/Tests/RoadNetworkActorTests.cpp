#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Components/BoxComponent.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "ProceduralRoadActor.h"
#include "ProceduralRoadJunctionActor.h"
#include "ProceduralMeshComponent.h"
#include "RoadNetworkActor.h"
#include "ScopedTransaction.h"
#include "Tests/RoadNetworkTestTypes.h"
#include "UObject/UnrealType.h"

namespace
{
	class FScopedRoadTestWorld
	{
	public:
		FScopedRoadTestWorld()
		{
			World = UWorld::CreateWorld(
				EWorldType::EditorPreview,
				false,
				MakeUniqueObjectName(
					GetTransientPackage(),
					UWorld::StaticClass(),
					TEXT("RoadPaintingTestWorld")));
			FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::EditorPreview);
			WorldContext.SetCurrentWorld(World);
		}

		~FScopedRoadTestWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}

		ARoadNetworkActor* SpawnNetwork() const
		{
			return World->SpawnActor<ARoadNetworkActor>();
		}

		int32 CountRoadActors() const
		{
			int32 Count = 0;
			for (TActorIterator<AProceduralRoadActor> Iterator(World); Iterator; ++Iterator)
			{
				++Count;
			}
			return Count;
		}

		AProceduralRoadActor* FindRoadActor() const
		{
			for (TActorIterator<AProceduralRoadActor> Iterator(World); Iterator; ++Iterator)
			{
				return *Iterator;
			}
			return nullptr;
		}

		AProceduralRoadJunctionActor* FindJunctionActor() const
		{
			for (TActorIterator<AProceduralRoadJunctionActor> Iterator(World); Iterator; ++Iterator)
			{
				return *Iterator;
			}
			return nullptr;
		}

	private:
		UWorld* World = nullptr;
	};

	void CaptureRoadEndpointVertices(
		UWorld* World,
		TMap<FName, TArray<FVector>>& OutVertices)
	{
		OutVertices.Reset();
		for (TActorIterator<AProceduralRoadActor> RoadIterator(World);
			RoadIterator;
			++RoadIterator)
		{
			AProceduralRoadActor* Road = *RoadIterator;
			TArray<FVector>& RoadVertices = OutVertices.FindOrAdd(Road->GetFName());
			for (ERoadSplineEndpoint Endpoint :
				{ERoadSplineEndpoint::Start, ERoadSplineEndpoint::End})
			{
				float TrimDistance = 0.0f;
				Road->GetJunctionTrimDistance(Endpoint, TrimDistance);
				FProceduralRoadJunctionEdgeGeometry Geometry;
				if (Road->GetJunctionEdgeGeometry(
					Endpoint,
					TrimDistance,
					Geometry))
				{
					RoadVertices.Append(Geometry.SurfacePoints);
				}
			}
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadPaintSimplificationTest,
	"SplineTools.RoadPainting.SimplifiesCollinearStroke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoadPaintSimplificationTest::RunTest(const FString& Parameters)
{
	FScopedRoadTestWorld TestWorld;
	ARoadNetworkActor* Network = TestWorld.SpawnNetwork();
	Network->SimplificationTolerance = 100.0f;
	const bool bAdded = Network->AddPaintedStroke(
		{
			FVector(0.0f, 0.0f, 0.0f),
			FVector(500.0f, 10.0f, 0.0f),
			FVector(1000.0f, 0.0f, 0.0f)
		},
		AProceduralRoadActor::StaticClass());
	TestTrue(TEXT("Stroke was accepted"), bAdded);
	TestEqual(TEXT("Collinear stroke has two control points"), Network->GetPoints().Num(), 2);
	TestEqual(TEXT("Collinear stroke has one link"), Network->GetLinks().Num(), 1);
	TestEqual(TEXT("One generated road run"), TestWorld.CountRoadActors(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadPaintUndoRemovesGeneratedActorsTest,
	"SplineTools.RoadPainting.UndoRemovesGeneratedActors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoadPaintUndoRemovesGeneratedActorsTest::RunTest(const FString& Parameters)
{
	FScopedRoadTestWorld TestWorld;
	ARoadNetworkActor* Network = TestWorld.SpawnNetwork();
	{
		const FScopedTransaction Transaction(
			NSLOCTEXT("RoadPaintingTests", "PaintRoadStroke", "Paint Road Stroke"));
		TestTrue(
			TEXT("Stroke was accepted"),
			Network->AddPaintedStroke(
				{FVector::ZeroVector, FVector(1000.0f, 0.0f, 0.0f)},
				AProceduralRoadActor::StaticClass()));
	}
	TestEqual(TEXT("Stroke generated one road"), TestWorld.CountRoadActors(), 1);
	AProceduralRoadActor* ManagedRoad = TestWorld.FindRoadActor();
	FStructProperty* ManagedNetworkProperty = FindFProperty<FStructProperty>(
		AProceduralRoadActor::StaticClass(),
		TEXT("ManagedRoadNetworkId"));
	TestNotNull(TEXT("Managed road exposes its network identity"), ManagedNetworkProperty);
	if (ManagedRoad && ManagedNetworkProperty)
	{
		const FGuid* ManagedNetworkId =
			ManagedNetworkProperty->ContainerPtrToValuePtr<FGuid>(ManagedRoad);
		AProceduralRoadActor* OrphanRoad =
			Network->GetWorld()->SpawnActor<AProceduralRoadActor>();
		OrphanRoad->SetManagedRoadIdentity(*ManagedNetworkId, FGuid::NewGuid());
		TestEqual(TEXT("Test setup created a managed orphan"), TestWorld.CountRoadActors(), 2);
		Network->RebuildAll();
		TestEqual(TEXT("Manual rebuild removes a managed orphan"), TestWorld.CountRoadActors(), 1);
	}

	GEditor->UndoTransaction();
	TestEqual(TEXT("Undo restored an empty graph"), Network->GetLinks().Num(), 0);
	FTSTicker::GetCoreTicker().Tick(0.1f);
	TestEqual(TEXT("Undo automatically removes the generated road"), TestWorld.CountRoadActors(), 0);

	GEditor->RedoTransaction();
	FTSTicker::GetCoreTicker().Tick(0.1f);
	TestEqual(TEXT("Redo restored the graph"), Network->GetLinks().Num(), 1);
	TestEqual(TEXT("Redo regenerated the road"), TestWorld.CountRoadActors(), 1);

	GEditor->UndoTransaction();
	FTSTicker::GetCoreTicker().Tick(0.1f);
	TestEqual(TEXT("A second undo remains clean"), TestWorld.CountRoadActors(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadPaintPreviewIsTimeSlicedTest,
	"SplineTools.RoadPainting.TimeSlicesStrokePreview",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoadPaintPreviewIsTimeSlicedTest::RunTest(const FString& Parameters)
{
	FScopedRoadTestWorld TestWorld;
	ARoadNetworkActor* Network = TestWorld.SpawnNetwork();
	Network->SimplificationTolerance = 0.0f;
	TArray<FVector> StrokePoints;
	for (int32 PointIndex = 0; PointIndex < 12; ++PointIndex)
	{
		StrokePoints.Add(FVector(
			PointIndex * 100.0f,
			(PointIndex % 2) * 100.0f,
			0.0f));
	}

	FRoadStrokePreviewState PreviewState;
	TArray<FVector> SimplifiedPoints;
	TArray<FVector> Intersections;
	int32 BuildCalls = 0;
	bool bComplete = false;
	while (!bComplete && BuildCalls < 100)
	{
		bComplete = Network->BuildStrokePreview(
			StrokePoints,
			PreviewState,
			SimplifiedPoints,
			Intersections);
		++BuildCalls;
	}

	TestTrue(TEXT("Stroke preview completes across bounded calls"), bComplete);
	TestTrue(TEXT("Stroke preview required more than one bounded call"), BuildCalls > 1);
	TestEqual(
		TEXT("Time-sliced preview preserves the simplified stroke"),
		SimplifiedPoints.Num(),
		StrokePoints.Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadPaintInsertPointTest,
	"SplineTools.RoadPainting.InsertsPointOnSelectedLink",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoadPaintInsertPointTest::RunTest(const FString& Parameters)
{
	FScopedRoadTestWorld TestWorld;
	ARoadNetworkActor* Network = TestWorld.SpawnNetwork();
	TestTrue(
		TEXT("Road stroke was accepted"),
		Network->AddPaintedStroke(
			{FVector::ZeroVector, FVector(1000.0f, 0.0f, 0.0f)},
			AProceduralRoadActor::StaticClass()));
	Network->SetSelection(FGuid(), Network->GetLinks()[0].Id);

	TestTrue(
		TEXT("Selected link accepts inserted point"),
		Network->InsertPointOnLink(Network->GetSelectedLinkId()));
	TestEqual(TEXT("Inserted point splits the link into two links"), Network->GetLinks().Num(), 2);
	TestTrue(TEXT("Inserted point is selected"), Network->GetSelectedPointId().IsValid());
	TestTrue(TEXT("Inserted graph remains valid"), [&Network]()
	{
		FString Errors;
		return Network->ValidateNetworkGraph(Errors);
	}());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadPaintMultiSelectionTest,
	"SplineTools.RoadPainting.SupportsMultiSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoadPaintMultiSelectionTest::RunTest(const FString& Parameters)
{
	FScopedRoadTestWorld TestWorld;
	ARoadNetworkActor* Network = TestWorld.SpawnNetwork();
	Network->SimplificationTolerance = 0.0f;
	TestTrue(
		TEXT("Road stroke was accepted"),
		Network->AddPaintedStroke(
			{
				FVector(0.0f, 0.0f, 0.0f),
				FVector(500.0f, 200.0f, 0.0f),
				FVector(1000.0f, 0.0f, 0.0f)
			},
			AProceduralRoadActor::StaticClass()));
	TestEqual(TEXT("Test road has three control points"), Network->GetPoints().Num(), 3);

	const FGuid FirstPointId = Network->GetPoints()[0].Id;
	const FGuid SecondPointId = Network->GetPoints()[1].Id;
	const FGuid FirstLinkId = Network->GetLinks()[0].Id;
	Network->SetSelection(
		{FirstPointId, SecondPointId},
		{FirstLinkId});
	TestEqual(TEXT("Multi-selection stores two points"), Network->GetSelectedPointIds().Num(), 2);
	TestEqual(TEXT("Multi-selection stores one link"), Network->GetSelectedLinkIds().Num(), 1);
	TestTrue(TEXT("First point is selected"), Network->IsPointSelected(FirstPointId));
	TestTrue(TEXT("Link is selected"), Network->IsLinkSelected(FirstLinkId));

	Network->SetSelection({FirstPointId, SecondPointId}, {});
	TestTrue(TEXT("Multi-selection can be deleted as one operation"), Network->DeleteSelection());
	TestEqual(TEXT("Deleting selected points removes connected links"), Network->GetLinks().Num(), 0);
	TestEqual(TEXT("Deleting selected points removes isolated points"), Network->GetPoints().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadPaintContinuousRunTest,
	"SplineTools.RoadPainting.MergesSameClassDegreeTwoRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoadPaintContinuousRunTest::RunTest(const FString& Parameters)
{
	FScopedRoadTestWorld TestWorld;
	ARoadNetworkActor* Network = TestWorld.SpawnNetwork();
	Network->SnapRadius = 300.0f;
	Network->AddPaintedStroke(
		{FVector(0.0f, 0.0f, 0.0f), FVector(1000.0f, 0.0f, 0.0f)},
		AProceduralRoadActor::StaticClass());
	Network->AddPaintedStroke(
		{FVector(1000.0f, 0.0f, 0.0f), FVector(2000.0f, 0.0f, 0.0f)},
		AProceduralRoadActor::StaticClass());
	TestEqual(TEXT("Connected strokes retain three points"), Network->GetPoints().Num(), 3);
	TestEqual(TEXT("Connected strokes retain two links"), Network->GetLinks().Num(), 2);
	TestEqual(TEXT("Degree-two same-class links generate one road"), TestWorld.CountRoadActors(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadPaintLandscapeConfigurationTest,
	"SplineTools.RoadPainting.ValidatesLandscapePaintConfiguration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoadPaintLandscapeConfigurationTest::RunTest(const FString& Parameters)
{
	TestNotNull(
		TEXT("Landscape paint rebuild is exposed as a separate editor action"),
		ARoadNetworkActor::StaticClass()->FindFunctionByName(
			GET_FUNCTION_NAME_CHECKED(ARoadNetworkActor, RebuildLandscapePaint)));
	TestNotNull(
		TEXT("Landscape paint settings are owned by procedural road classes"),
		FindFProperty<FProperty>(
			AProceduralRoadActor::StaticClass(),
			TEXT("LandscapePaintSettings")));
	FProperty* LegacyNetworkToggle = FindFProperty<FProperty>(
		ARoadNetworkActor::StaticClass(),
		TEXT("bPaintLandscapeMaterial"));
	TestNotNull(TEXT("Legacy network paint data remains loadable for serialization compatibility"), LegacyNetworkToggle);
	TestFalse(
		TEXT("Legacy network paint data is no longer editable"),
		LegacyNetworkToggle && LegacyNetworkToggle->HasAnyPropertyFlags(CPF_Edit));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadPaintOverpassTest,
	"SplineTools.RoadPainting.DoesNotJoinHeightSeparatedCrossing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoadPaintOverpassTest::RunTest(const FString& Parameters)
{
	FScopedRoadTestWorld TestWorld;
	ARoadNetworkActor* Network = TestWorld.SpawnNetwork();
	Network->MaximumJunctionHeightDifference = 200.0f;
	Network->AddPaintedStroke(
		{FVector(-1000.0f, 0.0f, 0.0f), FVector(1000.0f, 0.0f, 0.0f)},
		AProceduralRoadActor::StaticClass());
	Network->AddPaintedStroke(
		{FVector(0.0f, -1000.0f, 1000.0f), FVector(0.0f, 1000.0f, 1000.0f)},
		AProceduralRoadActor::StaticClass());
	TestEqual(TEXT("Separated crossing has four endpoints"), Network->GetPoints().Num(), 4);
	TestEqual(TEXT("Separated crossing has two links"), Network->GetLinks().Num(), 2);
	TestEqual(TEXT("Separated crossing generates two roads"), TestWorld.CountRoadActors(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadPaintEndpointSnapTest,
	"SplineTools.RoadPainting.SnapsEndpointToNetwork",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoadPaintEndpointSnapTest::RunTest(const FString& Parameters)
{
	FScopedRoadTestWorld TestWorld;
	ARoadNetworkActor* Network = TestWorld.SpawnNetwork();
	Network->SnapRadius = 300.0f;
	Network->AddPaintedStroke(
		{FVector(0.0f, 0.0f, 0.0f), FVector(1000.0f, 0.0f, 0.0f)},
		AProceduralRoadActor::StaticClass());
	Network->AddPaintedStroke(
		{FVector(1150.0f, 50.0f, 0.0f), FVector(2000.0f, 0.0f, 0.0f)},
		AProceduralRoadActor::StaticClass());
	TestEqual(TEXT("Snapped strokes share one endpoint"), Network->GetPoints().Num(), 3);
	TestEqual(TEXT("Snapped strokes create two links"), Network->GetLinks().Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadPaintCrossingTest,
	"SplineTools.RoadPainting.InsertsCrossingAndExactJunction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoadPaintCrossingTest::RunTest(const FString& Parameters)
{
	FScopedRoadTestWorld TestWorld;
	ARoadNetworkActor* Network = TestWorld.SpawnNetwork();
	Network->AddPaintedStroke(
		{FVector(-1000.0f, 0.0f, 0.0f), FVector(1000.0f, 0.0f, 0.0f)},
		AProceduralRoadActor::StaticClass());
	Network->AddPaintedStroke(
		{FVector(0.0f, -1000.0f, 0.0f), FVector(0.0f, 1000.0f, 0.0f)},
		AProceduralRoadActor::StaticClass());
	TestEqual(TEXT("Crossing inserts one shared point"), Network->GetPoints().Num(), 5);
	TestEqual(TEXT("Crossing splits both paths"), Network->GetLinks().Num(), 4);
	AProceduralRoadJunctionActor* Junction = TestWorld.FindJunctionActor();
	TestNotNull(TEXT("Degree-four crossing generates a junction"), Junction);
	if (Junction)
	{
		TestEqual(TEXT("Junction owns four exact endpoints"), Junction->GetRoadConnections().Num(), 4);
		UProceduralMeshComponent* JunctionMesh =
			Junction->FindComponentByClass<UProceduralMeshComponent>();
		TestNotNull(TEXT("Junction has a procedural mesh"), JunctionMesh);
		if (JunctionMesh)
		{
			const FProcMeshSection* SurfaceSection = JunctionMesh->GetProcMeshSection(0);
			const FProcMeshSection* BlendSection = JunctionMesh->GetProcMeshSection(1);
			TestNotNull(TEXT("Junction has a terrain-sampled surface section"), SurfaceSection);
			TestNotNull(TEXT("Junction has a ground-blend skirt section"), BlendSection);
			if (SurfaceSection)
			{
				TestTrue(
					TEXT("Junction surface contains intermediate terrain samples"),
					SurfaceSection->ProcVertexBuffer.Num() > 13);
			}
			if (SurfaceSection && BlendSection)
			{
				for (const FProceduralRoadJunctionConnection& Connection :
					Junction->GetRoadConnections())
				{
					float TrimDistance = 0.0f;
					Connection.Road->GetJunctionTrimDistance(
						Connection.Endpoint,
						TrimDistance);
					FProceduralRoadJunctionEdgeGeometry EdgeGeometry;
					TestTrue(
						TEXT("Road exposes its generated junction edge"),
						Connection.Road->GetJunctionEdgeGeometry(
							Connection.Endpoint,
							TrimDistance,
							EdgeGeometry));
					TestTrue(
						TEXT("Junction edge comes from the cached road mesh"),
						EdgeGeometry.bUsesCachedMesh);
					for (const FVector& SurfacePoint : EdgeGeometry.SurfacePoints)
					{
						bool bFoundSurfacePoint = false;
						for (const FProcMeshVertex& JunctionVertex :
							SurfaceSection->ProcVertexBuffer)
						{
							const FVector JunctionWorldPosition =
								JunctionMesh->GetComponentTransform().TransformPosition(
									JunctionVertex.Position);
							if (JunctionWorldPosition.Equals(SurfacePoint, 0.01f))
							{
								bFoundSurfacePoint = true;
								break;
							}
						}
						TestTrue(
							TEXT("Junction surface reuses an exact road endpoint vertex"),
							bFoundSurfacePoint);
					}
					for (int32 EdgePointIndex = 0;
						EdgePointIndex + 1 < EdgeGeometry.SurfacePoints.Num();
						++EdgePointIndex)
					{
						bool bSkirtCrossesRoadMouth = false;
						for (int32 TriangleIndex = 0;
							TriangleIndex + 2 < BlendSection->ProcIndexBuffer.Num();
							TriangleIndex += 3)
						{
							bool bContainsFirstPoint = false;
							bool bContainsSecondPoint = false;
							for (int32 CornerIndex = 0; CornerIndex < 3; ++CornerIndex)
							{
								const int32 VertexIndex =
									BlendSection->ProcIndexBuffer[TriangleIndex + CornerIndex];
								const FVector JunctionWorldPosition =
									JunctionMesh->GetComponentTransform().TransformPosition(
										BlendSection->ProcVertexBuffer[VertexIndex].Position);
								bContainsFirstPoint |= JunctionWorldPosition.Equals(
									EdgeGeometry.SurfacePoints[EdgePointIndex],
									0.01f);
								bContainsSecondPoint |= JunctionWorldPosition.Equals(
									EdgeGeometry.SurfacePoints[EdgePointIndex + 1],
									0.01f);
							}
							bSkirtCrossesRoadMouth |=
								bContainsFirstPoint && bContainsSecondPoint;
						}
						TestFalse(
							TEXT("Junction skirt leaves the road mouth open"),
							bSkirtCrossesRoadMouth);
					}
					if (EdgeGeometry.bHasSideFlaps)
					{
						bool bFoundLeftFlap = false;
						bool bFoundRightFlap = false;
						for (const FProcMeshVertex& JunctionVertex :
							BlendSection->ProcVertexBuffer)
						{
							const FVector JunctionWorldPosition =
								JunctionMesh->GetComponentTransform().TransformPosition(
									JunctionVertex.Position);
							bFoundLeftFlap |= JunctionWorldPosition.Equals(
								EdgeGeometry.LeftFlapPoint,
								0.01f);
							bFoundRightFlap |= JunctionWorldPosition.Equals(
								EdgeGeometry.RightFlapPoint,
								0.01f);
						}
						TestTrue(
							TEXT("Junction skirt reuses the road's left flap vertex"),
							bFoundLeftFlap);
						TestTrue(
							TEXT("Junction skirt reuses the road's right flap vertex"),
							bFoundRightFlap);
					}
				}
			}
		}

		AActor* TerrainDepression =
			Network->GetWorld()->SpawnActor<AActor>();
		TerrainDepression->SetActorLocation(FVector(0.0f, 0.0f, -500.0f));
		UBoxComponent* DepressionCollision = NewObject<UBoxComponent>(
			TerrainDepression,
			TEXT("TerrainDepressionCollision"));
		TerrainDepression->SetRootComponent(DepressionCollision);
		DepressionCollision->SetBoxExtent(FVector(100.0f, 100.0f, 10.0f));
		DepressionCollision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		DepressionCollision->SetCollisionObjectType(ECC_WorldStatic);
		DepressionCollision->SetCollisionResponseToAllChannels(ECR_Ignore);
		DepressionCollision->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
		DepressionCollision->RegisterComponent();
		Junction->RebuildJunctionMesh();

		UProceduralMeshComponent* SupportedJunctionMesh =
			Junction->FindComponentByClass<UProceduralMeshComponent>();
		const FProcMeshSection* SupportedSurfaceSection =
			SupportedJunctionMesh
				? SupportedJunctionMesh->GetProcMeshSection(0)
				: nullptr;
		TestNotNull(
			TEXT("Road-supported junction surface remains generated"),
			SupportedSurfaceSection);
		if (SupportedJunctionMesh
			&& SupportedSurfaceSection
			&& !SupportedSurfaceSection->ProcVertexBuffer.IsEmpty())
		{
			const FVector SupportedCenter =
				SupportedJunctionMesh->GetComponentTransform().TransformPosition(
					SupportedSurfaceSection->ProcVertexBuffer[0].Position);
			TestTrue(
				TEXT("Terrain depression cannot collapse the road-supported center"),
				FMath::IsNearlyZero(SupportedCenter.Z, 1.0f));
		}

		TMap<FName, TArray<FVector>> InitialRoadVertices;
		CaptureRoadEndpointVertices(Network->GetWorld(), InitialRoadVertices);
		SupportedJunctionMesh->ClearAllMeshSections();
		Network->RebuildDirty();
		TestNotNull(
			TEXT("Explicit dirty rebuild refreshes the junction cache"),
			SupportedJunctionMesh->GetProcMeshSection(0));
		Network->RebuildAll();
		Network->RebuildAll();

		TMap<FName, TArray<FVector>> RebuiltRoadVertices;
		CaptureRoadEndpointVertices(Network->GetWorld(), RebuiltRoadVertices);
		TestEqual(
			TEXT("Repeated rebuild preserves the managed road actor count"),
			RebuiltRoadVertices.Num(),
			InitialRoadVertices.Num());
		for (const TPair<FName, TArray<FVector>>& InitialRoad : InitialRoadVertices)
		{
			const TArray<FVector>* RebuiltVertices =
				RebuiltRoadVertices.Find(InitialRoad.Key);
			TestNotNull(
				TEXT("Repeated rebuild preserves each managed road actor"),
				RebuiltVertices);
			if (!RebuiltVertices)
			{
				continue;
			}
			TestEqual(
				TEXT("Repeated rebuild preserves road endpoint vertex count"),
				RebuiltVertices->Num(),
				InitialRoad.Value.Num());
			for (int32 VertexIndex = 0;
				VertexIndex < FMath::Min(
					RebuiltVertices->Num(),
					InitialRoad.Value.Num());
				++VertexIndex)
			{
				TestTrue(
					TEXT("Repeated rebuild is geometrically idempotent"),
					(*RebuiltVertices)[VertexIndex].Equals(
						InitialRoad.Value[VertexIndex],
						0.01f));
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadPaintSelfIntersectionTest,
	"SplineTools.RoadPainting.InsertsSelfIntersection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoadPaintSelfIntersectionTest::RunTest(const FString& Parameters)
{
	FScopedRoadTestWorld TestWorld;
	ARoadNetworkActor* Network = TestWorld.SpawnNetwork();
	Network->SimplificationTolerance = 0.0f;
	Network->AddPaintedStroke(
		{
			FVector(-1000.0f, -1000.0f, 0.0f),
			FVector(1000.0f, 1000.0f, 0.0f),
			FVector(-1000.0f, 1000.0f, 0.0f),
			FVector(1000.0f, -1000.0f, 0.0f)
		},
		AProceduralRoadActor::StaticClass());
	TestEqual(TEXT("Self crossing inserts one shared point"), Network->GetPoints().Num(), 5);
	TestEqual(TEXT("Self crossing orders split links"), Network->GetLinks().Num(), 5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadPaintDuplicateSampleTest,
	"SplineTools.RoadPainting.RemovesDuplicateSamples",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoadPaintDuplicateSampleTest::RunTest(const FString& Parameters)
{
	FScopedRoadTestWorld TestWorld;
	ARoadNetworkActor* Network = TestWorld.SpawnNetwork();
	Network->SimplificationTolerance = 0.0f;
	const bool bAdded = Network->AddPaintedStroke(
		{
			FVector(0.0f, 0.0f, 0.0f),
			FVector(0.0f, 0.0f, 0.0f),
			FVector(1000.0f, 0.0f, 0.0f)
		},
		AProceduralRoadActor::StaticClass());
	TestTrue(TEXT("Stroke with duplicate samples remains valid"), bAdded);
	TestEqual(TEXT("Duplicate sample does not create a graph point"), Network->GetPoints().Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadPaintDeleteDegreeTwoTest,
	"SplineTools.RoadPainting.DeleteDegreeTwoReconnectsCompatibleLinks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoadPaintDeleteDegreeTwoTest::RunTest(const FString& Parameters)
{
	FScopedRoadTestWorld TestWorld;
	ARoadNetworkActor* Network = TestWorld.SpawnNetwork();
	Network->SimplificationTolerance = 0.0f;
	Network->AddPaintedStroke(
		{
			FVector(0.0f, 0.0f, 0.0f),
			FVector(500.0f, 500.0f, 0.0f),
			FVector(1000.0f, 0.0f, 0.0f)
		},
		AProceduralRoadActor::StaticClass());
	const FGuid MiddlePointId = Network->GetPoints()[1].Id;
	TestTrue(TEXT("Interior point is deleted"), Network->DeletePoint(MiddlePointId));
	TestEqual(TEXT("Compatible neighbors reconnect"), Network->GetLinks().Num(), 1);
	TestEqual(TEXT("Only endpoints remain"), Network->GetPoints().Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadPaintMixedClassTest,
	"SplineTools.RoadPainting.SplitsMixedClassTransition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoadPaintMixedClassTest::RunTest(const FString& Parameters)
{
	FScopedRoadTestWorld TestWorld;
	ARoadNetworkActor* Network = TestWorld.SpawnNetwork();
	Network->AddPaintedStroke(
		{FVector(0.0f, 0.0f, 0.0f), FVector(1000.0f, 0.0f, 0.0f)},
		AProceduralRoadActor::StaticClass());
	Network->AddPaintedStroke(
		{FVector(1000.0f, 0.0f, 0.0f), FVector(2000.0f, 0.0f, 0.0f)},
		ARoadNetworkAlternateRoadActor::StaticClass());
	TestEqual(TEXT("Mixed classes remain separate roads"), TestWorld.CountRoadActors(), 2);
	AProceduralRoadJunctionActor* Junction = TestWorld.FindJunctionActor();
	TestNotNull(TEXT("Degree-two class transition creates a junction"), Junction);
	if (Junction)
	{
		TestEqual(TEXT("Transition junction has two exact endpoints"), Junction->GetRoadConnections().Num(), 2);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadPaintAdoptionTest,
	"SplineTools.RoadPainting.AdoptionPreservesSplineMetadataAndActor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoadPaintAdoptionTest::RunTest(const FString& Parameters)
{
	FScopedRoadTestWorld TestWorld;
	ARoadNetworkActor* Network = TestWorld.SpawnNetwork();
	AProceduralRoadActor* Road = Network->GetWorld()->SpawnActor<AProceduralRoadActor>();
	TArray<FProceduralRoadSplinePoint> AuthoredPoints;
	FProceduralRoadSplinePoint& FirstPoint = AuthoredPoints.AddDefaulted_GetRef();
	FirstPoint.WorldLocation = FVector(0.0f, 0.0f, 0.0f);
	FirstPoint.WorldArriveTangent = FVector(0.0f, -500.0f, 0.0f);
	FirstPoint.WorldLeaveTangent = FVector(0.0f, 500.0f, 0.0f);
	FirstPoint.Type = ESplinePointType::CurveCustomTangent;
	FProceduralRoadSplinePoint& SecondPoint = AuthoredPoints.AddDefaulted_GetRef();
	SecondPoint.WorldLocation = FVector(1000.0f, 0.0f, 0.0f);
	SecondPoint.Type = ESplinePointType::Linear;
	FProceduralRoadSplinePoint& ThirdPoint = AuthoredPoints.AddDefaulted_GetRef();
	ThirdPoint.WorldLocation = FVector(500.0f, 1000.0f, 0.0f);
	ThirdPoint.WorldArriveTangent = FVector(-500.0f, 0.0f, 0.0f);
	ThirdPoint.WorldLeaveTangent = FVector(500.0f, 0.0f, 0.0f);
	ThirdPoint.Type = ESplinePointType::CurveCustomTangent;
	Road->SetRoadSplinePoints(AuthoredPoints, true, false);

	GEditor->SelectNone(false, true, false);
	GEditor->SelectActor(Road, true, false, true);
	TestTrue(TEXT("Selected road is adopted"), Network->AdoptSelectedRoads());
	TestEqual(TEXT("Adoption keeps the original road actor"), TestWorld.CountRoadActors(), 1);
	TestTrue(TEXT("Original actor receives a managed run ID"), Road->GetManagedRoadRunId().IsValid());
	TestTrue(TEXT("Closed loop is preserved"), Road->IsRoadSplineClosedLoop());
	TestEqual(TEXT("Closed three-point road has three links"), Network->GetLinks().Num(), 3);
	TestEqual(TEXT("Adopted control point type is preserved"), Network->GetPoints()[0].Type.GetValue(), ESplinePointType::CurveCustomTangent);
	TestTrue(
		TEXT("Custom tangent metadata is retained"),
		Network->GetLinks().ContainsByPredicate(
			[](const FRoadNetworkLink& Link)
			{
				return Link.bHasCustomTangents && !Link.StartLeaveTangent.IsNearlyZero();
			}));
	GEditor->SelectNone(false, true, false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadPaintPartialAdoptionTest,
	"SplineTools.RoadPainting.AdoptionRejectsPartialJunction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoadPaintPartialAdoptionTest::RunTest(const FString& Parameters)
{
	FScopedRoadTestWorld TestWorld;
	ARoadNetworkActor* Network = TestWorld.SpawnNetwork();
	AProceduralRoadActor* SelectedRoad = Network->GetWorld()->SpawnActor<AProceduralRoadActor>();
	AProceduralRoadActor* UnselectedRoad = Network->GetWorld()->SpawnActor<AProceduralRoadActor>();
	AProceduralRoadJunctionActor* Junction =
		Network->GetWorld()->SpawnActor<AProceduralRoadJunctionActor>();
	TArray<FProceduralRoadJunctionConnection> Connections;
	FProceduralRoadJunctionConnection& FirstConnection = Connections.AddDefaulted_GetRef();
	FirstConnection.Road = SelectedRoad;
	FirstConnection.Endpoint = ERoadSplineEndpoint::End;
	FProceduralRoadJunctionConnection& SecondConnection = Connections.AddDefaulted_GetRef();
	SecondConnection.Road = UnselectedRoad;
	SecondConnection.Endpoint = ERoadSplineEndpoint::Start;
	Junction->SetManagedConnections(Connections, FGuid(), FGuid(), false);

	GEditor->SelectNone(false, true, false);
	GEditor->SelectActor(SelectedRoad, true, false, true);
	GEditor->SelectActor(Junction, true, false, true);
	AddExpectedError(
		TEXT("Cannot partially adopt junction"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	TestFalse(
		TEXT("Junction adoption refuses an unselected connected road"),
		Network->AdoptSelectedRoads());
	TestEqual(TEXT("Rejected adoption leaves graph unchanged"), Network->GetLinks().Num(), 0);
	GEditor->SelectNone(false, true, false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadPaintNearbyJunctionTrimTest,
	"SplineTools.RoadPainting.BudgetsTrimBetweenNearbyJunctions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoadPaintNearbyJunctionTrimTest::RunTest(const FString& Parameters)
{
	FScopedRoadTestWorld TestWorld;
	ARoadNetworkActor* Network = TestWorld.SpawnNetwork();
	AProceduralRoadActor* Road = Network->GetWorld()->SpawnActor<AProceduralRoadActor>();
	TArray<FProceduralRoadSplinePoint> RoadPoints;
	FProceduralRoadSplinePoint& StartPoint = RoadPoints.AddDefaulted_GetRef();
	StartPoint.WorldLocation = FVector::ZeroVector;
	StartPoint.Type = ESplinePointType::Linear;
	FProceduralRoadSplinePoint& EndPoint = RoadPoints.AddDefaulted_GetRef();
	EndPoint.WorldLocation = FVector(500.0f, 0.0f, 0.0f);
	EndPoint.Type = ESplinePointType::Linear;
	Road->SetRoadSplinePoints(RoadPoints, false, false);

	AProceduralRoadJunctionActor* StartJunction =
		Network->GetWorld()->SpawnActor<AProceduralRoadJunctionActor>();
	AProceduralRoadJunctionActor* EndJunction =
		Network->GetWorld()->SpawnActor<AProceduralRoadJunctionActor>();
	EndJunction->SetActorLocation(EndPoint.WorldLocation);
	FProceduralRoadJunctionConnection StartConnection;
	StartConnection.Road = Road;
	StartConnection.Endpoint = ERoadSplineEndpoint::Start;
	StartConnection.TrimDistance = 300.0f;
	FProceduralRoadJunctionConnection EndConnection;
	EndConnection.Road = Road;
	EndConnection.Endpoint = ERoadSplineEndpoint::End;
	EndConnection.TrimDistance = 300.0f;
	StartJunction->SetManagedConnections(
		{StartConnection},
		FGuid::NewGuid(),
		FGuid::NewGuid(),
		false);
	EndJunction->SetManagedConnections(
		{EndConnection},
		FGuid::NewGuid(),
		FGuid::NewGuid(),
		false);
	StartJunction->RebuildJunction();
	EndJunction->RebuildJunction();

	float StartTrimDistance = 0.0f;
	float EndTrimDistance = 0.0f;
	TestTrue(
		TEXT("Start junction owns its road endpoint"),
		Road->GetJunctionTrimDistance(
			ERoadSplineEndpoint::Start,
			StartTrimDistance));
	TestTrue(
		TEXT("End junction owns its road endpoint"),
		Road->GetJunctionTrimDistance(
			ERoadSplineEndpoint::End,
			EndTrimDistance));
	TestEqual(TEXT("Nearby start trim is reduced"), StartTrimDistance, 200.0f);
	TestEqual(TEXT("Nearby end trim is reduced"), EndTrimDistance, 200.0f);
	return true;
}

#endif
