#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "ProceduralRoadActor.h"
#include "UObject/UnrealType.h"

namespace
{
	class FScopedRoadsideTestWorld
	{
	public:
		FScopedRoadsideTestWorld()
		{
			World = UWorld::CreateWorld(
				EWorldType::EditorPreview,
				false,
				MakeUniqueObjectName(
					GetTransientPackage(),
					UWorld::StaticClass(),
					TEXT("RoadsideTestWorld")));
			FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::EditorPreview);
			WorldContext.SetCurrentWorld(World);
		}

		~FScopedRoadsideTestWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}

		AProceduralRoadActor* SpawnRoad(bool bClosedLoop = false) const
		{
			AProceduralRoadActor* Road = World->SpawnActor<AProceduralRoadActor>();
			TArray<FProceduralRoadSplinePoint> Points;
			FProceduralRoadSplinePoint& StartPoint = Points.AddDefaulted_GetRef();
			StartPoint.WorldLocation = FVector::ZeroVector;
			FProceduralRoadSplinePoint& EndPoint = Points.AddDefaulted_GetRef();
			EndPoint.WorldLocation = FVector(10000.0f, 0.0f, 0.0f);
			Road->SetRoadSplinePoints(Points, bClosedLoop, false);
			return Road;
		}

	private:
		UWorld* World = nullptr;
	};

	UStaticMesh* CreateTestMesh(AActor* Owner, const TCHAR* Name)
	{
		return NewObject<UStaticMesh>(Owner, FName(Name), RF_Transient);
	}

	bool SetRoadsideDefinitions(
		AProceduralRoadActor* Road,
		const TArray<FProceduralRoadsideMeshDefinition>& Definitions)
	{
		FArrayProperty* DefinitionsProperty = FindFProperty<FArrayProperty>(
			AProceduralRoadActor::StaticClass(),
			TEXT("RoadsideMeshDefinitions"));
		if (!DefinitionsProperty)
		{
			return false;
		}

		FStructProperty* DefinitionProperty = CastField<FStructProperty>(DefinitionsProperty->Inner);
		if (!DefinitionProperty)
		{
			return false;
		}

		FScriptArrayHelper DefinitionsHelper(
			DefinitionsProperty,
			DefinitionsProperty->ContainerPtrToValuePtr<void>(Road));
		DefinitionsHelper.EmptyValues();
		DefinitionsHelper.AddValues(Definitions.Num());
		for (int32 DefinitionIndex = 0; DefinitionIndex < Definitions.Num(); ++DefinitionIndex)
		{
			DefinitionProperty->CopyCompleteValue(
				DefinitionsHelper.GetRawPtr(DefinitionIndex),
				&Definitions[DefinitionIndex]);
		}
		return true;
	}

	UHierarchicalInstancedStaticMeshComponent* FindRoadsideComponent(
		AProceduralRoadActor* Road,
		const TCHAR* RoleName)
	{
		TArray<UHierarchicalInstancedStaticMeshComponent*> Components;
		Road->GetComponents(Components);
		const FString ExpectedName = FString::Printf(TEXT("RoadsideMesh_0_%s"), RoleName);
		for (UHierarchicalInstancedStaticMeshComponent* Component : Components)
		{
			if (Component && Component->GetName() == ExpectedName)
			{
				return Component;
			}
		}
		return nullptr;
	}

	int32 CountRoadsideComponents(AProceduralRoadActor* Road)
	{
		TArray<UHierarchicalInstancedStaticMeshComponent*> Components;
		Road->GetComponents(Components);
		int32 Count = 0;
		for (UHierarchicalInstancedStaticMeshComponent* Component : Components)
		{
			if (Component && Component->GetName().StartsWith(TEXT("RoadsideMesh_")))
			{
				++Count;
			}
		}
		return Count;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProceduralRoadsideExactSpacingTest,
	"SplineTools.Roadside.ExactSpacingAndCollision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProceduralRoadsideExactSpacingTest::RunTest(const FString& Parameters)
{
	FScopedRoadsideTestWorld TestWorld;
	AProceduralRoadActor* Road = TestWorld.SpawnRoad();
	FProceduralRoadsideMeshDefinition Definition;
	Definition.MiddleMesh = CreateTestMesh(Road, TEXT("EdgePostMesh"));
	Definition.Side = ERoadsideMeshSide::Both;
	Definition.Orientation = ERoadsideMeshOrientation::FaceLane;
	Definition.DistanceFromRoadCenter = 600.0f;
	Definition.MeshSpacing = 5000.0f;
	Definition.bEnableCollision = false;
	TestTrue(
		TEXT("Roadside definitions property is available"),
		SetRoadsideDefinitions(Road, {Definition}));

	Road->RebuildSplineTool();
	UHierarchicalInstancedStaticMeshComponent* MiddleComponent =
		FindRoadsideComponent(Road, TEXT("Middle"));
	TestNotNull(TEXT("Middle roadside HISM was generated"), MiddleComponent);
	if (!MiddleComponent)
	{
		return false;
	}

	TestEqual(TEXT("Both sides share two centered distance samples"), MiddleComponent->GetInstanceCount(), 4);
	TestEqual(TEXT("Collision can be disabled per definition"), MiddleComponent->GetCollisionEnabled(), ECollisionEnabled::NoCollision);
	TArray<float> LongitudinalDistances;
	for (int32 InstanceIndex = 0; InstanceIndex < MiddleComponent->GetInstanceCount(); ++InstanceIndex)
	{
		FTransform InstanceTransform;
		MiddleComponent->GetInstanceTransform(InstanceIndex, InstanceTransform, true);
		LongitudinalDistances.Add(InstanceTransform.GetLocation().X);
		TestEqual(
			TEXT("Edge post remains at the configured center distance"),
			static_cast<int32>(FMath::RoundToInt(FMath::Abs(InstanceTransform.GetLocation().Y))),
			600);
	}
	LongitudinalDistances.Sort();
	TestEqual(TEXT("First edge-post pair is inset by half the spacing"), static_cast<int32>(FMath::RoundToInt(LongitudinalDistances[0])), 2500);
	TestEqual(TEXT("Last edge-post pair is inset by half the spacing"), static_cast<int32>(FMath::RoundToInt(LongitudinalDistances[2])), 7500);

	FTransform LeftTransform;
	FTransform RightTransform;
	MiddleComponent->GetInstanceTransform(0, LeftTransform, true);
	MiddleComponent->GetInstanceTransform(3, RightTransform, true);
	TestTrue(
		TEXT("Lane-facing instances on opposite sides face opposite directions"),
		FVector::DotProduct(
			LeftTransform.GetUnitAxis(EAxis::X),
			RightTransform.GetUnitAxis(EAxis::X)) < -0.99f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProceduralRoadsideBoundaryAndRebuildTest,
	"SplineTools.Roadside.BoundariesAndRebuild",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProceduralRoadsideBoundaryAndRebuildTest::RunTest(const FString& Parameters)
{
	FScopedRoadsideTestWorld TestWorld;
	AProceduralRoadActor* Road = TestWorld.SpawnRoad();
	UStaticMesh* Mesh = CreateTestMesh(Road, TEXT("GuardrailMesh"));
	FProceduralRoadsideMeshDefinition Definition;
	Definition.StartMesh = Mesh;
	Definition.MiddleMesh = Mesh;
	Definition.EndMesh = Mesh;
	Definition.Side = ERoadsideMeshSide::Right;
	Definition.MeshSpacing = 3000.0f;
	Definition.bEnableCollision = true;
	TestTrue(TEXT("Guardrail definition was assigned"), SetRoadsideDefinitions(Road, {Definition}));

	Road->RebuildSplineTool();
	UHierarchicalInstancedStaticMeshComponent* StartComponent =
		FindRoadsideComponent(Road, TEXT("Start"));
	UHierarchicalInstancedStaticMeshComponent* MiddleComponent =
		FindRoadsideComponent(Road, TEXT("Middle"));
	UHierarchicalInstancedStaticMeshComponent* EndComponent =
		FindRoadsideComponent(Road, TEXT("End"));
	TestNotNull(TEXT("Guardrail start HISM was generated"), StartComponent);
	TestNotNull(TEXT("Guardrail middle HISM was generated"), MiddleComponent);
	TestNotNull(TEXT("Guardrail end HISM was generated"), EndComponent);
	if (!StartComponent || !MiddleComponent || !EndComponent)
	{
		return false;
	}

	TestEqual(TEXT("Start mesh is placed at the effective start"), StartComponent->GetInstanceCount(), 1);
	TestEqual(TEXT("Middle meshes stay strictly between caps"), MiddleComponent->GetInstanceCount(), 3);
	TestEqual(TEXT("End mesh is placed at the effective end"), EndComponent->GetInstanceCount(), 1);
	TestEqual(TEXT("Guardrail collision is enabled"), MiddleComponent->GetCollisionEnabled(), ECollisionEnabled::QueryAndPhysics);
	const int32 InitialComponentCount = CountRoadsideComponents(Road);
	Road->RebuildSplineTool();
	TestEqual(TEXT("Rebuilding does not duplicate roadside HISM components"), CountRoadsideComponents(Road), InitialComponentCount);
	TestEqual(
		TEXT("Rebuilding preserves middle instance count"),
		FindRoadsideComponent(Road, TEXT("Middle"))->GetInstanceCount(),
		3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProceduralRoadsideClosedLoopTest,
	"SplineTools.Roadside.ClosedLoopUsesMiddleOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProceduralRoadsideClosedLoopTest::RunTest(const FString& Parameters)
{
	FScopedRoadsideTestWorld TestWorld;
	AProceduralRoadActor* Road = TestWorld.SpawnRoad(true);
	UStaticMesh* Mesh = CreateTestMesh(Road, TEXT("LoopRoadsideMesh"));
	FProceduralRoadsideMeshDefinition Definition;
	Definition.StartMesh = Mesh;
	Definition.MiddleMesh = Mesh;
	Definition.EndMesh = Mesh;
	Definition.MeshSpacing = 5000.0f;
	TestTrue(TEXT("Closed-loop definition was assigned"), SetRoadsideDefinitions(Road, {Definition}));

	Road->RebuildSplineTool();
	TestTrue(TEXT("Closed loop has no start HISM"), FindRoadsideComponent(Road, TEXT("Start")) == nullptr);
	TestTrue(TEXT("Closed loop has no end HISM"), FindRoadsideComponent(Road, TEXT("End")) == nullptr);
	UHierarchicalInstancedStaticMeshComponent* MiddleComponent =
		FindRoadsideComponent(Road, TEXT("Middle"));
	TestNotNull(TEXT("Closed loop has a middle HISM"), MiddleComponent);
	if (MiddleComponent)
	{
		TestTrue(TEXT("Closed loop generated middle instances"), MiddleComponent->GetInstanceCount() > 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProceduralRoadsideDeterminismTest,
	"SplineTools.Roadside.DeterministicVariance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProceduralRoadsideDeterminismTest::RunTest(const FString& Parameters)
{
	FScopedRoadsideTestWorld TestWorld;
	AProceduralRoadActor* Road = TestWorld.SpawnRoad();
	FProceduralRoadsideMeshDefinition Definition;
	Definition.MiddleMesh = CreateTestMesh(Road, TEXT("RandomRoadsideMesh"));
	Definition.Side = ERoadsideMeshSide::Both;
	Definition.Orientation = ERoadsideMeshOrientation::Random;
	Definition.DistanceFromRoadCenter = 600.0f;
	Definition.DistanceFromRoadCenterVariance = 50.0f;
	Definition.HeightOffsetVariance = 25.0f;
	Definition.MeshSpacing = 2500.0f;
	Definition.MeshSpacingJitter = 500.0f;
	Definition.RandomYawRangeDegrees = 90.0f;
	Definition.bAlignToLandscape = true;
	Definition.bEnableCollision = false;
	Definition.RandomSeed = 9876;
	TestTrue(TEXT("Deterministic variance definition was assigned"), SetRoadsideDefinitions(Road, {Definition}));

	Road->RebuildSplineTool();
	UHierarchicalInstancedStaticMeshComponent* FirstComponent =
		FindRoadsideComponent(Road, TEXT("Middle"));
	TestNotNull(TEXT("Variance roadside HISM was generated"), FirstComponent);
	if (!FirstComponent)
	{
		return false;
	}

	TArray<FTransform> FirstTransforms;
	FirstTransforms.SetNum(FirstComponent->GetInstanceCount());
	for (int32 InstanceIndex = 0; InstanceIndex < FirstComponent->GetInstanceCount(); ++InstanceIndex)
	{
		FirstComponent->GetInstanceTransform(InstanceIndex, FirstTransforms[InstanceIndex], true);
	}

	Road->RebuildSplineTool();
	UHierarchicalInstancedStaticMeshComponent* SecondComponent =
		FindRoadsideComponent(Road, TEXT("Middle"));
	TestNotNull(TEXT("Variance roadside HISM survives rebuild"), SecondComponent);
	if (!SecondComponent)
	{
		return false;
	}

	TestEqual(TEXT("Deterministic variance preserves instance count"), SecondComponent->GetInstanceCount(), FirstTransforms.Num());
	for (int32 InstanceIndex = 0; InstanceIndex < FirstTransforms.Num(); ++InstanceIndex)
	{
		FTransform SecondTransform;
		SecondComponent->GetInstanceTransform(InstanceIndex, SecondTransform, true);
		TestTrue(
			TEXT("Deterministic variance preserves each transform"),
			FirstTransforms[InstanceIndex].Equals(SecondTransform, 0.01f));
	}
	return true;
}

#endif
