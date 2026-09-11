#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Components/BoxComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MaterialDomain.h"
#include "ProceduralRoadJunctionActor.h"
#include "ProceduralMeshComponent.h"
#include "UObject/UnrealType.h"

namespace
{
	void SetBool(UObject* Object, const TCHAR* Name, bool Value)
	{
		FindFProperty<FBoolProperty>(Object->GetClass(), Name)->SetPropertyValue_InContainer(Object, Value);
	}

	void SetFloat(UObject* Object, const TCHAR* Name, float Value)
	{
		FindFProperty<FFloatProperty>(Object->GetClass(), Name)->SetPropertyValue_InContainer(Object, Value);
	}

	void SetMaterial(UObject* Object, const TCHAR* Name, UMaterialInterface* Value)
	{
		FindFProperty<FObjectPropertyBase>(Object->GetClass(), Name)->SetObjectPropertyValue_InContainer(Object, Value);
	}

	class FJunctionFixture
	{
	public:
		FJunctionFixture()
		{
			World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
			GEngine->CreateNewWorldContext(EWorldType::EditorPreview).SetCurrentWorld(World);
			Junction = World->SpawnActor<AProceduralRoadJunctionActor>();
			SetBool(Junction, TEXT("bAlignToTerrain"), false);
			SetBool(Junction, TEXT("bGenerateCollision"), false);
			SetFloat(Junction, TEXT("RoadMouthPadding"), 0.0f);
			for (const FVector& End : {FVector(-4000, 0, 0), FVector(4000, 0, 0), FVector(0, 4000, 0)})
			{
				AProceduralRoadActor* Road = World->SpawnActor<AProceduralRoadActor>();
				Road->SetEditorRebuildDeferred(true);
				SetBool(Road, TEXT("bAlignToTerrain"), false);
				SetBool(Road, TEXT("bGenerateCollision"), false);
				SetBool(Road, TEXT("bGenerateSideLines"), true);
				SetMaterial(Road, TEXT("RoadLineMaterial"), UMaterial::GetDefaultMaterial(MD_Surface));
				TArray<FProceduralRoadSplinePoint> Points;
				Points.AddDefaulted(2);
				Points[0].WorldLocation = FVector::ZeroVector;
				Points[1].WorldLocation = End;
				Road->SetRoadSplinePoints(Points, false, true);
				FProceduralRoadJunctionConnection Connection;
				Connection.Road = Road;
				Connection.Endpoint = ERoadSplineEndpoint::Start;
				Connection.TrimDistance = 600;
				Connections.Add(Connection);
			}
			Junction->SetManagedConnections(Connections, FGuid::NewGuid(), FGuid::NewGuid(), true);
		}

		~FJunctionFixture()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}

		UProceduralMeshComponent* Mesh() const { return Junction->FindComponentByClass<UProceduralMeshComponent>(); }

		void AddGroundBox(const FVector& Location, const FVector& Extent, const FRotator& Rotation = FRotator::ZeroRotator)
		{
			AActor* Ground = World->SpawnActor<AActor>();
			UBoxComponent* Box = NewObject<UBoxComponent>(Ground);
			Ground->SetRootComponent(Box);
			Ground->SetActorLocation(Location);
			Ground->SetActorRotation(Rotation);
			Box->SetBoxExtent(Extent);
			Box->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Box->SetCollisionResponseToAllChannels(ECR_Ignore);
			Box->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
			Box->RegisterComponent();
		}

		UWorld* World = nullptr;
		AProceduralRoadJunctionActor* Junction = nullptr;
		TArray<FProceduralRoadJunctionConnection> Connections;
	};

	double MaximumSurfaceHeight(UProceduralMeshComponent* Mesh)
	{
		double Height = -TNumericLimits<double>::Max();
		for (const FProcMeshVertex& Vertex : Mesh->GetProcMeshSection(0)->ProcVertexBuffer)
		{
			Height = FMath::Max(Height, Mesh->GetComponentTransform().TransformPosition(Vertex.Position).Z);
		}
		return Height;
	}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJunctionGroundSmoothingTest, "SplineTools.Junction.DenseSmoothedGround", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJunctionGroundSmoothingTest::RunTest(const FString& Parameters)
{
	FJunctionFixture Fixture;
	const int32 DenseCount = Fixture.Mesh()->GetProcMeshSection(0)->ProcVertexBuffer.Num();
	SetFloat(Fixture.Junction, TEXT("TerrainSampleSpacing"), 300);
	Fixture.Junction->RebuildJunction();
	TestTrue(TEXT("Larger sample spacing reduces interior surface density"), DenseCount > Fixture.Mesh()->GetProcMeshSection(0)->ProcVertexBuffer.Num());
	SetFloat(Fixture.Junction, TEXT("TerrainSampleSpacing"), 75);
	SetBool(Fixture.Junction, TEXT("bAlignToTerrain"), true);
	Fixture.AddGroundBox(FVector(0, 0, -10), FVector(5000, 5000, 10));
	Fixture.AddGroundBox(FVector(200, 0, 20), FVector(110, 110, 20));
	SetFloat(Fixture.Junction, TEXT("InteriorSmoothingStrength"), 0);
	Fixture.Junction->RebuildJunction();
	const double RoughHeight = MaximumSurfaceHeight(Fixture.Mesh());
	TestTrue(TEXT("Fixture contains a sampled ground bump"), RoughHeight > 20);
	SetFloat(Fixture.Junction, TEXT("InteriorSmoothingStrength"), 0.65f);
	Fixture.Junction->RebuildJunction();
	TestTrue(TEXT("Smoothing reduces the local bump"), MaximumSurfaceHeight(Fixture.Mesh()) < RoughHeight - 1);
	TestTrue(TEXT("Smoothed patch retains ground height variation"), MaximumSurfaceHeight(Fixture.Mesh()) > 3);
	for (const FProceduralRoadJunctionConnection& Connection : Fixture.Connections)
	{
		FProceduralRoadJunctionEdgeGeometry Edge;
		Connection.Road->GetJunctionEdgeGeometry(Connection.Endpoint, Connection.TrimDistance, Edge);
		for (const FVector& Position : Edge.SurfacePoints)
		{
			TestTrue(TEXT("Smoothing preserves exact cached road seam"), Fixture.Mesh()->GetProcMeshSection(0)->ProcVertexBuffer.ContainsByPredicate([&](const FProcMeshVertex& Vertex) { return Fixture.Mesh()->GetComponentTransform().TransformPosition(Vertex.Position).Equals(Position, 0.01f); }));
		}
	}
	const TArray<FProcMeshVertex> Baked = Fixture.Mesh()->GetProcMeshSection(0)->ProcVertexBuffer;
	Fixture.Junction->RebuildJunction();
	TestEqual(TEXT("Repeated terrain rebuild retains vertex count"), Fixture.Mesh()->GetProcMeshSection(0)->ProcVertexBuffer.Num(), Baked.Num());
	for (int32 Index = 0; Index < Baked.Num(); ++Index)
	{
		TestTrue(TEXT("Repeated terrain rebuild is geometrically stable"), Fixture.Mesh()->GetProcMeshSection(0)->ProcVertexBuffer[Index].Position.Equals(Baked[Index].Position, 0.01f));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJunctionLandscapeCenterTest, "SplineTools.Junction.LandscapeCenter", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJunctionLandscapeCenterTest::RunTest(const FString& Parameters)
{
	FJunctionFixture Fixture;
	SetBool(Fixture.Junction, TEXT("bAlignToTerrain"), true);
	SetFloat(Fixture.Junction, TEXT("InteriorSmoothingStrength"), 1.0f);
	Fixture.Junction->SetActorLocation(FVector(900, 900, 1000));
	Fixture.AddGroundBox(FVector(0, 0, -10), FVector(5000, 5000, 10));
	Fixture.Junction->RebuildJunction();

	FProcMeshSection* Section = Fixture.Mesh()->GetProcMeshSection(0);
	TestNotNull(TEXT("Junction generated a surface section"), Section);
	if (!Section)
	{
		return false;
	}

	const FVector Center = Fixture.Mesh()->GetComponentTransform().TransformPosition(
		Section->ProcVertexBuffer[0].Position);
	TestTrue(
		TEXT("Junction center follows the connected road convergence"),
		Center.Equals(FVector(0, 0, 3), 0.01f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJunctionTerrainDepressionTest, "SplineTools.Junction.EdgeSupportedSurface", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJunctionTerrainDepressionTest::RunTest(const FString& Parameters)
{
	FJunctionFixture Fixture;
	SetBool(Fixture.Junction, TEXT("bAlignToTerrain"), true);
	SetFloat(Fixture.Junction, TEXT("InteriorSmoothingStrength"), 0.0f);
	Fixture.AddGroundBox(FVector(0, 0, -500), FVector(500, 500, 10));
	Fixture.Junction->RebuildJunction();

	FProcMeshSection* Section = Fixture.Mesh()->GetProcMeshSection(0);
	TestNotNull(TEXT("Junction generated a surface section"), Section);
	if (!Section)
	{
		return false;
	}
	TestTrue(
		TEXT("Junction center stays on the edge-supported plane over a depression"),
		Fixture.Mesh()->GetComponentTransform().TransformPosition(
			Section->ProcVertexBuffer[0].Position).Z >= -0.01);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJunctionCenterTerrainSupportTest, "SplineTools.Junction.CenterTerrainSupport", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJunctionCenterTerrainSupportTest::RunTest(const FString& Parameters)
{
	FJunctionFixture Fixture;
	SetBool(Fixture.Junction, TEXT("bAlignToTerrain"), true);
	SetFloat(Fixture.Junction, TEXT("InteriorSmoothingStrength"), 0.0f);
	Fixture.AddGroundBox(FVector(0, 0, 25), FVector(5000, 5000, 25), FRotator(1.0f, 0.0f, 0.0f));
	Fixture.Junction->RebuildJunction();

	FProcMeshSection* Section = Fixture.Mesh()->GetProcMeshSection(0);
	TestNotNull(TEXT("Junction generated a surface section"), Section);
	if (!Section)
	{
		return false;
	}

	const FVector Center = Fixture.Mesh()->GetComponentTransform().TransformPosition(
		Section->ProcVertexBuffer[0].Position);
	TestTrue(TEXT("Junction center stays above the angled terrain"), Center.Z > 25.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJunctionRoadMouthPaddingTest, "SplineTools.Junction.RoadMouthPadding", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJunctionRoadMouthPaddingTest::RunTest(const FString& Parameters)
{
	FJunctionFixture Fixture;
	SetFloat(Fixture.Junction, TEXT("RoadMouthPadding"), 150.0f);
	Fixture.Junction->RebuildJunction();

	for (const FProceduralRoadJunctionConnection& Connection : Fixture.Connections)
	{
		float TrimDistance = 0.0f;
		TestTrue(
			TEXT("Connected road exposes its junction trim"),
			Connection.Road->GetJunctionTrimDistance(
				Connection.Endpoint,
				TrimDistance));
		TestEqual(TEXT("Road mouth starts after the configured padding"), TrimDistance, 750.0f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJunctionSurfaceUVTest, "SplineTools.Junction.SurfaceUVs", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJunctionSurfaceUVTest::RunTest(const FString& Parameters)
{
	FJunctionFixture Fixture;
	FProcMeshSection* Section = Fixture.Mesh()->GetProcMeshSection(0);
	TestNotNull(TEXT("Junction generated a surface section"), Section);
	if (!Section)
	{
		return false;
	}

	for (const FProcMeshVertex& Vertex : Section->ProcVertexBuffer)
	{
		const FVector Position = Fixture.Mesh()->GetComponentTransform().TransformPosition(Vertex.Position);
		const FVector2D ExpectedUV(Position.X / 400.0f, Position.Y / 400.0f);
		TestTrue(TEXT("Junction surface keeps uniform planar UVs"), Vertex.UV0.Equals(ExpectedUV, 0.001f));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJunctionEdgeMarkingsTest, "SplineTools.Junction.EdgeMarkings", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJunctionEdgeMarkingsTest::RunTest(const FString& Parameters)
{
	FJunctionFixture Fixture;
	UMaterialInstanceDynamic* Alternate = UMaterialInstanceDynamic::Create(UMaterial::GetDefaultMaterial(MD_Surface), Fixture.Junction);
	SetMaterial(Fixture.Connections[0].Road, TEXT("RoadLineMaterial"), Alternate);
	SetFloat(Fixture.Connections[0].Road, TEXT("RoadLineWidth"), 20);
	SetFloat(Fixture.Connections[0].Road, TEXT("RoadLineUVWorldLength"), 60);
	Fixture.Connections[0].Road->RebuildRoad();
	Fixture.Junction->RebuildJunction();
	TestEqual(TEXT("T junction has three corners, each with two material halves"), Fixture.Mesh()->GetNumSections(), 8);
	for (const FProceduralRoadJunctionConnection& Connection : Fixture.Connections)
	{
		for (bool bLeft : {true, false})
		{
			FProceduralRoadLineEdge Edge;
			TestTrue(TEXT("Enabled road exports its side-line seam"), Connection.Road->GetJunctionSideLineEdge(Connection.Endpoint, Connection.TrimDistance, bLeft, Edge));
			bool bFoundInner = false;
			bool bFoundOuter = false;
			for (int32 SectionIndex = 2; SectionIndex < Fixture.Mesh()->GetNumSections(); ++SectionIndex)
			{
				FProcMeshSection* Section = Fixture.Mesh()->GetProcMeshSection(SectionIndex);
				TestFalse(TEXT("Markings do not generate collision"), Section->bEnableCollision);
				for (const FProcMeshVertex& Vertex : Section->ProcVertexBuffer)
				{
					const FVector Position = Fixture.Mesh()->GetComponentTransform().TransformPosition(Vertex.Position);
					if (Position.Equals(Edge.Inner, 0.01f))
					{
						bFoundInner = true;
						TestTrue(TEXT("Inner seam UV matches road"), Vertex.UV0.Equals(FVector2D(1 - Edge.OuterU, Edge.V), 0.001f));
						TestEqual(TEXT("Seam inherits road material"), Fixture.Mesh()->GetMaterial(SectionIndex), Edge.Material);
					}
					if (Position.Equals(Edge.Outer, 0.01f))
					{
						bFoundOuter = true;
						TestTrue(TEXT("Outer seam UV matches road"), Vertex.UV0.Equals(FVector2D(Edge.OuterU, Edge.V), 0.001f));
					}
				}
			}
			TestTrue(TEXT("Corner joins exact inner road marking vertex"), bFoundInner);
			TestTrue(TEXT("Corner joins exact outer road marking vertex"), bFoundOuter);
		}
	}
	AProceduralRoadJunctionActor* Duplicate = DuplicateObject<AProceduralRoadJunctionActor>(Fixture.Junction, Fixture.World->PersistentLevel);
	TestEqual(TEXT("Duplicated junction restores cached markings"), Duplicate->FindComponentByClass<UProceduralMeshComponent>()->GetNumSections(), Fixture.Mesh()->GetNumSections());
	SetBool(Fixture.Connections[2].Road, TEXT("bGenerateSideLines"), false);
	Fixture.Connections[2].Road->RebuildRoad();
	Fixture.Junction->RebuildJunction();
	TestEqual(TEXT("Unmarked branch leaves only the opposite marked edge"), Fixture.Mesh()->GetNumSections(), 4);
	for (const FProceduralRoadJunctionConnection& Connection : Fixture.Connections)
	{
		SetBool(Connection.Road, TEXT("bGenerateSideLines"), false);
		Connection.Road->RebuildRoad();
	}
	Fixture.Junction->RebuildJunction();
	TestEqual(TEXT("Unmarked roads remove cached junction markings"), Fixture.Mesh()->GetNumSections(), 2);
	return true;
}

#endif
