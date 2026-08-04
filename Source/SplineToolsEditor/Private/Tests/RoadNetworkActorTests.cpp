#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "ProceduralRoadActor.h"
#include "ProceduralRoadJunctionActor.h"
#include "RoadNetworkActor.h"
#include "Tests/RoadNetworkTestTypes.h"

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
		}

		~FScopedRoadTestWorld()
		{
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

#endif
