#include "SplineToolActorBase.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SplineComponent.h"
#include "Containers/Ticker.h"
#include "Engine/World.h"
#include "UObject/UnrealType.h"

ASplineToolActorBase::ASplineToolActorBase()
{
	PrimaryActorTick.bCanEverTick = false;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	ToolSpline = CreateDefaultSubobject<USplineComponent>(TEXT("Spline"));
	ToolSpline->SetupAttachment(SceneRoot);
	ToolSpline->SetClosedLoop(false);
}

void ASplineToolActorBase::BeginPlay()
{
	Super::BeginPlay();
	if (ShouldRebuildInGameWorld())
	{
		RebuildSplineTool();
	}
}

void ASplineToolActorBase::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	if (GetWorld() && GetWorld()->IsGameWorld())
	{
		if (ShouldRebuildInGameWorld())
		{
			RebuildSplineTool();
		}
		return;
	}

#if WITH_EDITOR
	QueueEditorRebuild();
#endif
}

void ASplineToolActorBase::RebuildSplineTool()
{
	UpdateSplineSettings();
	ResetGeneratedContent();

	if (!IsSplineUsable())
	{
		return;
	}

	GenerateSplineContent();
	FinalizeGeneratedContent();
}

void ASplineToolActorBase::UpdateSplineSettings()
{
}

void ASplineToolActorBase::ResetGeneratedContent()
{
	for (int32 ComponentIndex = GeneratedInstanceComponents.Num() - 1; ComponentIndex >= 0; --ComponentIndex)
	{
		if (!GeneratedInstanceComponents[ComponentIndex])
		{
			continue;
		}

		RemoveInstanceComponent(GeneratedInstanceComponents[ComponentIndex]);
		GeneratedInstanceComponents[ComponentIndex]->DestroyComponent();
	}

	GeneratedInstanceComponents.Empty();
}

void ASplineToolActorBase::GenerateSplineContent()
{
}

void ASplineToolActorBase::FinalizeGeneratedContent()
{
	for (int32 ComponentIndex = 0; ComponentIndex < GeneratedInstanceComponents.Num(); ++ComponentIndex)
	{
		UHierarchicalInstancedStaticMeshComponent* MeshComponent = GeneratedInstanceComponents[ComponentIndex];
		if (!MeshComponent)
		{
			continue;
		}

		if (!MeshComponent->IsRegistered())
		{
			MeshComponent->RegisterComponent();
		}

		MeshComponent->BuildTreeIfOutdated(false, true);
		MeshComponent->MarkRenderStateDirty();
	}
}

bool ASplineToolActorBase::ShouldRebuildInGameWorld() const
{
	return true;
}

UHierarchicalInstancedStaticMeshComponent* ASplineToolActorBase::CreateGeneratedHISM(const FString& ComponentName, UStaticMesh* StaticMesh)
{
	if (!StaticMesh)
	{
		return nullptr;
	}

	UHierarchicalInstancedStaticMeshComponent* MeshComponent = NewObject<UHierarchicalInstancedStaticMeshComponent>(this, *ComponentName);
	AddInstanceComponent(MeshComponent);
	MeshComponent->SetMobility(EComponentMobility::Static);
	MeshComponent->bAutoRebuildTreeOnInstanceChanges = false;
	MeshComponent->SetStaticMesh(StaticMesh);
	MeshComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	MeshComponent->SetCollisionProfileName(TEXT("BlockAll"));
	MeshComponent->SetupAttachment(SceneRoot);
	GeneratedInstanceComponents.Add(MeshComponent);
	return MeshComponent;
}

bool ASplineToolActorBase::IsSplineUsable() const
{
	return ToolSpline->GetNumberOfSplinePoints() > 1 && ToolSpline->GetSplineLength() > KINDA_SMALL_NUMBER;
}

FTransform ASplineToolActorBase::GetSplineTransformAtDistance(float DistanceAlongSpline) const
{
	return ToolSpline->GetTransformAtDistanceAlongSpline(
		DistanceAlongSpline,
		ESplineCoordinateSpace::World,
		true
	);
}

FTransform ASplineToolActorBase::BuildSplineInstanceTransform(
	float DistanceAlongSpline,
	const FVector& InstanceScale,
	const FVector& LocationOffset,
	const FRotator& RotationOffset
) const
{
	FTransform InstanceTransform = GetSplineTransformAtDistance(DistanceAlongSpline);
	InstanceTransform.ConcatenateRotation(RotationOffset.Quaternion());
	InstanceTransform.AddToTranslation(InstanceTransform.GetRotation().RotateVector(LocationOffset));
	InstanceTransform.SetScale3D(InstanceTransform.GetScale3D() * InstanceScale);
	return InstanceTransform;
}

void ASplineToolActorBase::AddSplineInstance(
	UHierarchicalInstancedStaticMeshComponent* MeshComponent,
	float DistanceAlongSpline,
	const FVector& InstanceScale,
	const FVector& LocationOffset,
	const FRotator& RotationOffset
) const
{
	if (!MeshComponent)
	{
		return;
	}

	FTransform InstanceTransform = BuildSplineInstanceTransform(
		DistanceAlongSpline,
		InstanceScale,
		LocationOffset,
		RotationOffset
	);
	MeshComponent->AddInstance(InstanceTransform, true);
}

#if WITH_EDITOR
void ASplineToolActorBase::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);
	if (bFinished)
	{
		CancelQueuedEditorRebuild();
		if (!IsTemplate())
		{
			RebuildSplineTool();
		}
		return;
	}

	QueueEditorRebuild();
}

void ASplineToolActorBase::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	QueueEditorRebuild();
}

void ASplineToolActorBase::PostEditUndo()
{
	Super::PostEditUndo();
	QueueEditorRebuild();
}

void ASplineToolActorBase::QueueEditorRebuild()
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
					RebuildSplineTool();
				}

				return false;
			}
		),
		0.15f
	);
}

void ASplineToolActorBase::CancelQueuedEditorRebuild()
{
	if (!EditorRebuildTickerHandle.IsValid())
	{
		return;
	}

	FTSTicker::GetCoreTicker().RemoveTicker(EditorRebuildTickerHandle);
	EditorRebuildTickerHandle.Reset();
}
#endif
