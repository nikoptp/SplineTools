#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "RoadNoPassing.h"
#include "ProceduralRoadActor.h"
#include "ProceduralMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Materials/Material.h"
#include "MaterialDomain.h"
#include "UObject/UnrealType.h"

namespace
{
	TArray<FRoadNoPassingSample> MakeSamples(float Bend, float Hill, float Grade = 0.0f)
	{
		TArray<FRoadNoPassingSample> Samples;
		for (int32 Index = 0; Index <= 40; ++Index)
		{
			const float X = Index * 500.0f;
			const float Centered = X - 10000.0f;
			Samples.Add({X, FVector(X, Bend * Centered * Centered / 10000.0f, Hill * (1.0f - FMath::Square(Centered / 10000.0f)) + Grade * X)});
		}
		return Samples;
	}

	bool HasSide(const TArray<FRoadNoPassingRange>& Ranges, int32 Side)
	{
		return Ranges.ContainsByPredicate([Side](const FRoadNoPassingRange& Range) { return Range.SideSign == Side; });
	}

	void SetBool(AProceduralRoadActor* Road, const TCHAR* Name, bool Value)
	{
		FindFProperty<FBoolProperty>(Road->GetClass(), Name)->SetPropertyValue_InContainer(Road, Value);
	}

	void SetFloat(AProceduralRoadActor* Road, const TCHAR* Name, float Value)
	{
		FindFProperty<FFloatProperty>(Road->GetClass(), Name)->SetPropertyValue_InContainer(Road, Value);
	}

	void SetMaterial(AProceduralRoadActor* Road, const TCHAR* Name, UMaterialInterface* Value)
	{
		FindFProperty<FObjectPropertyBase>(Road->GetClass(), Name)->SetObjectPropertyValue_InContainer(Road, Value);
	}

	int32 CountVertices(AProceduralRoadActor* Road, int32 SectionIndex)
	{
		TArray<UProceduralMeshComponent*> Chunks;
		Road->GetComponents(Chunks);
		int32 Count = 0;
		for (UProceduralMeshComponent* Chunk : Chunks)
		{
			if (FProcMeshSection* Section = Chunk->GetProcMeshSection(SectionIndex))
			{
				Count += Section->ProcVertexBuffer.Num();
			}
		}
		return Count;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoadNoPassingClassificationTest, "SplineTools.Road.NoPassing.Classification", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoadNoPassingClassificationTest::RunTest(const FString& Parameters)
{
	FRoadNoPassingSettings Settings;
	TestTrue(TEXT("Straight road is unrestricted"), RoadNoPassing::Analyze(MakeSamples(0, 0), Settings).IsEmpty());
	TestTrue(TEXT("Constant grade is unrestricted"), RoadNoPassing::Analyze(MakeSamples(0, 0, 0.2f), Settings).IsEmpty());
	TestTrue(TEXT("Valley is unrestricted"), RoadNoPassing::Analyze(MakeSamples(0, -5000), Settings).IsEmpty());
	const TArray<FRoadNoPassingRange> Right = RoadNoPassing::Analyze(MakeSamples(1, 0), Settings);
	TestTrue(TEXT("Right bend restricts inside right lane"), HasSide(Right, 1));
	TestFalse(TEXT("Right bend leaves outside lane unrestricted"), HasSide(Right, -1));
	const TArray<FRoadNoPassingRange> Left = RoadNoPassing::Analyze(MakeSamples(-1, 0), Settings);
	TestTrue(TEXT("Mirrored bend restricts left lane"), HasSide(Left, -1));
	TestFalse(TEXT("Mirrored bend leaves right lane unrestricted"), HasSide(Left, 1));
	const TArray<FRoadNoPassingRange> Crest = RoadNoPassing::Analyze(MakeSamples(0, 5000), Settings);
	TestTrue(TEXT("Crest restricts both lanes"), HasSide(Crest, -1) && HasSide(Crest, 1));
	const TArray<FRoadNoPassingRange> Combined = RoadNoPassing::Analyze(MakeSamples(1, 5000), Settings);
	TestTrue(TEXT("Crest and bend combine both restrictions"), HasSide(Combined, -1) && HasSide(Combined, 1));
	TestTrue(TEXT("Empty input is unrestricted"), RoadNoPassing::Analyze({}, Settings).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoadNoPassingIntervalsTest, "SplineTools.Road.NoPassing.Intervals", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoadNoPassingIntervalsTest::RunTest(const FString& Parameters)
{
	FRoadNoPassingSettings Settings;
	Settings.AdvanceDistance = 0;
	Settings.MinimumLength = 0;
	const TArray<FRoadNoPassingSample> Samples = MakeSamples(1, 0);
	const TArray<FRoadNoPassingRange> Base = RoadNoPassing::Analyze(Samples, Settings);
	if (!TestEqual(TEXT("One merged bend interval"), Base.Num(), 1))
	{
		return false;
	}
	Settings.AdvanceDistance = 1000;
	const TArray<FRoadNoPassingRange> Advanced = RoadNoPassing::Analyze(Samples, Settings);
	TestEqual(TEXT("Right traffic gets advance toward lower distance"), Advanced[0].Start, FMath::Max(0.0f, Base[0].Start - 1000));
	TestEqual(TEXT("Advance leaves exit unchanged"), Advanced[0].End, Base[0].End);
	TArray<FRoadNoPassingSample> Reversed;
	for (int32 Index = Samples.Num() - 1; Index >= 0; --Index)
	{
		Reversed.Add({Samples.Last().Distance - Samples[Index].Distance, Samples[Index].Position});
	}
	const TArray<FRoadNoPassingRange> ReverseRanges = RoadNoPassing::Analyze(Reversed, Settings);
	if (TestEqual(TEXT("Reversal preserves interval count"), ReverseRanges.Num(), Advanced.Num()))
	{
		TestEqual(TEXT("Reversed spline changes side sign"), ReverseRanges[0].SideSign, -Advanced[0].SideSign);
		TestEqual(TEXT("Physical restriction start preserved"), ReverseRanges[0].Start, Samples.Last().Distance - Advanced[0].End);
		TestEqual(TEXT("Physical restriction end preserved"), ReverseRanges[0].End, Samples.Last().Distance - Advanced[0].Start);
	}
	Settings.MinimumLength = 16000;
	const TArray<FRoadNoPassingRange> Padded = RoadNoPassing::Analyze(Samples, Settings);
	TestTrue(TEXT("Short restriction extended to minimum length"), Padded[0].End - Padded[0].Start >= 16000);
	Settings.AdvanceDistance = 50000;
	for (const FRoadNoPassingRange& Range : RoadNoPassing::Analyze(Samples, Settings))
	{
		TestTrue(TEXT("Extension clipped to effective road"), Range.Start >= 0 && Range.End <= 20000);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoadNoPassingMeshTest, "SplineTools.Road.NoPassing.MeshAndCache", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoadNoPassingMeshTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
	GEngine->CreateNewWorldContext(EWorldType::EditorPreview).SetCurrentWorld(World);
	AProceduralRoadActor* Road = World->SpawnActor<AProceduralRoadActor>();
	SetBool(Road, TEXT("bAlignToTerrain"), false);
	SetBool(Road, TEXT("bGenerateCollision"), false);
	SetBool(Road, TEXT("bGenerateCenterLine"), true);
	SetBool(Road, TEXT("bCenterLineHasGaps"), true);
	SetMaterial(Road, TEXT("CenterLineMaterial"), UMaterial::GetDefaultMaterial(MD_Surface));
	SetMaterial(Road, TEXT("NoPassingLineMaterial"), UMaterial::GetDefaultMaterial(MD_Surface));
	TArray<FProceduralRoadSplinePoint> Points;
	for (const FRoadNoPassingSample& Sample : MakeSamples(0, 5000))
	{
		FProceduralRoadSplinePoint& Point = Points.AddDefaulted_GetRef();
		Point.WorldLocation = Sample.Position;
	}
	Road->SetRoadSplinePoints(Points, false, true);
	TestTrue(TEXT("Crest creates yellow ribbons"), CountVertices(Road, 4) > 0);
	const int32 YellowCount = CountVertices(Road, 4);
	Road->RebuildSplineTool();
	TestEqual(TEXT("Unchanged rebuild preserves yellow cache"), CountVertices(Road, 4), YellowCount);
	AProceduralRoadActor* Duplicate = DuplicateObject<AProceduralRoadActor>(Road, World->PersistentLevel);
	TestEqual(TEXT("Duplication restores serialized yellow sections"), CountVertices(Duplicate, 4), YellowCount);
	World->WorldType = EWorldType::PIE;
	Duplicate->DispatchBeginPlay();
	TestEqual(TEXT("PIE BeginPlay retains cache without analysis ranges"), CountVertices(Duplicate, 4), YellowCount);
	World->WorldType = EWorldType::EditorPreview;

	// Dash UVs remain anchored to spline distance, including intervals cut by chunk seams.
	TArray<UProceduralMeshComponent*> Chunks;
	Road->GetComponents(Chunks);
	TestTrue(TEXT("Fixture spans multiple chunks"), Chunks.Num() > 1);
	for (UProceduralMeshComponent* Chunk : Chunks)
	{
		if (FProcMeshSection* White = Chunk->GetProcMeshSection(3))
		{
			for (const FProcMeshVertex& Vertex : White->ProcVertexBuffer)
			{
				const float Phase = FMath::Fmod(Vertex.UV0.Y * 100.0f, 600.0f);
				TestTrue(TEXT("White vertices stay inside global dash intervals"), Phase <= 300.1f || Phase >= 599.9f);
			}
		}
	}
	// Every yellow seam vertex is reproduced by its adjoining chunk.
	for (UProceduralMeshComponent* Chunk : Chunks)
	{
		FProcMeshSection* Yellow = Chunk->GetProcMeshSection(4);
		if (!Yellow || Yellow->ProcVertexBuffer.IsEmpty())
		{
			continue;
		}
		for (const FProcMeshVertex& Vertex : Yellow->ProcVertexBuffer)
		{
			const float Distance = Vertex.UV0.Y * 100.0f;
			if (Distance < 1 || !FMath::IsNearlyZero(FMath::Fmod(Distance, 5000.0f), 0.01f) || Distance >= Road->GetRoadSplineLength() - 1)
			{
				continue;
			}
			bool bFoundMatchingSeam = false;
			for (UProceduralMeshComponent* Other : Chunks)
			{
				if (Other != Chunk)
				{
					if (FProcMeshSection* OtherYellow = Other->GetProcMeshSection(4))
					{
						bFoundMatchingSeam |= OtherYellow->ProcVertexBuffer.ContainsByPredicate([&Vertex](const FProcMeshVertex& Candidate) { return Candidate.Position.Equals(Vertex.Position, 0.01f) && Candidate.UV0.Equals(Vertex.UV0, 0.001f); });
					}
				}
			}
			TestTrue(TEXT("Adjacent chunks share yellow seam positions and UVs"), bFoundMatchingSeam);
		}
	}
	AActor* TrimOwner = World->SpawnActor<AActor>();
	Road->SetJunctionTrim(ERoadSplineEndpoint::Start, 3000, TrimOwner);
	Road->SetJunctionTrim(ERoadSplineEndpoint::End, 3000, TrimOwner);
	Road->GetComponents(Chunks);
	for (UProceduralMeshComponent* Chunk : Chunks)
	{
		if (FProcMeshSection* Yellow = Chunk->GetProcMeshSection(4))
		{
			for (const FProcMeshVertex& Vertex : Yellow->ProcVertexBuffer)
			{
				TestTrue(TEXT("Yellow obeys junction trims"), Vertex.UV0.Y * 100 >= 2999.9f && Vertex.UV0.Y * 100 <= Road->GetRoadSplineLength() - 2999.9f);
			}
		}
	}
	Road->ClearJunctionTrim(ERoadSplineEndpoint::Start, TrimOwner);
	Road->ClearJunctionTrim(ERoadSplineEndpoint::End, TrimOwner);
	SetFloat(Road, TEXT("NoPassingCrestThresholdDegrees"), 180);
	Road->RebuildSplineTool();
	TestEqual(TEXT("Sensitivity change invalidates obsolete yellow cache"), CountVertices(Road, 4), 0);
	SetFloat(Road, TEXT("NoPassingCrestThresholdDegrees"), 4);
	Road->RebuildSplineTool();
	TestTrue(TEXT("Sensitivity restore regenerates yellow"), CountVertices(Road, 4) > 0);
	SetMaterial(Road, TEXT("NoPassingLineMaterial"), nullptr);
	Road->RebuildSplineTool();
	TestEqual(TEXT("Clearing material removes yellow"), CountVertices(Road, 4), 0);
	TestTrue(TEXT("Clearing material preserves white"), CountVertices(Road, 3) > 0);
	SetMaterial(Road, TEXT("NoPassingLineMaterial"), UMaterial::GetDefaultMaterial(MD_Surface));
	SetBool(Road, TEXT("bGenerateCenterLine"), false);
	Road->RebuildSplineTool();
	TestEqual(TEXT("Disabling center lines removes yellow"), CountVertices(Road, 4), 0);
	TestEqual(TEXT("Disabling center lines removes white"), CountVertices(Road, 3), 0);
	// A corner far beyond the first chunk must still invalidate its approach markings.
	SetBool(Road, TEXT("bGenerateCenterLine"), true);
	SetFloat(Road, TEXT("NoPassingAdvanceDistance"), 20000);
	Points.Reset();
	for (int32 Index = 0; Index <= 5; ++Index)
	{
		FProceduralRoadSplinePoint& Point = Points.AddDefaulted_GetRef();
		Point.WorldLocation = FVector(Index * 5000, 0, 0);
		Point.Type = ESplinePointType::Linear;
	}
	Road->SetRoadSplinePoints(Points, false, true);
	TestEqual(TEXT("Straight fixture starts without yellow"), CountVertices(Road, 4), 0);
	UProceduralMeshComponent* FirstChunk = CastChecked<UProceduralMeshComponent>(Road->GetDefaultSubobjectByName(TEXT("RoadMesh")));
	const FVector OriginalStart = FirstChunk->GetProcMeshSection(0)->ProcVertexBuffer[0].Position;
	Points.Last().WorldLocation.Y = 10000;
	Road->SetRoadSplinePoints(Points, false, true);
	TestEqual(TEXT("Distant edit preserves first chunk surface position"), FirstChunk->GetProcMeshSection(0)->ProcVertexBuffer[0].Position, OriginalStart);
	TestNotNull(TEXT("Distant bend invalidates first chunk approach cache"), FirstChunk->GetProcMeshSection(4));
	TestTrue(TEXT("Single restriction retains white marking"), CountVertices(Road, 3) > 0);
	USplineComponent* Spline = Road->FindComponentByClass<USplineComponent>();
	Road->GetComponents(Chunks);
	for (UProceduralMeshComponent* Chunk : Chunks)
	{
		if (FProcMeshSection* Yellow = Chunk->GetProcMeshSection(4))
		{
			for (const FProcMeshVertex& Vertex : Yellow->ProcVertexBuffer)
			{
				const FVector Center = Spline->GetLocationAtDistanceAlongSpline(Vertex.UV0.Y * 100, ESplineCoordinateSpace::World);
				const FVector Right = Spline->GetRightVectorAtDistanceAlongSpline(Vertex.UV0.Y * 100, ESplineCoordinateSpace::World);
				TestTrue(TEXT("Single yellow ribbon lies in the inside lane"), FVector::DotProduct(Chunk->GetComponentTransform().TransformPosition(Vertex.Position) - Center, Right) > 0);
			}
		}
	}
	Points.Last().WorldLocation.Y = 0;
	Road->SetRoadSplinePoints(Points, false, true);
	TestEqual(TEXT("Removing distant corner clears approach restrictions"), CountVertices(Road, 4), 0);
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

#endif
