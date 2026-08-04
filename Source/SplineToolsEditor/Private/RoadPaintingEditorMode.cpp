#include "RoadPaintingEditorMode.h"

#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/Selection.h"
#include "InteractiveToolManager.h"
#include "RoadNetworkActor.h"
#include "RoadPaintingEditorModeCommands.h"
#include "RoadPaintingEditorModeToolkit.h"
#include "ScopedTransaction.h"
#include "Tools/RoadPaintTool.h"
#include "Tools/RoadSelectTool.h"

#define LOCTEXT_NAMESPACE "RoadPaintingEditorMode"

const FEditorModeID URoadPaintingEditorMode::ModeId = TEXT("EM_RoadPaintingEditorMode");
const FString URoadPaintingEditorMode::DrawToolName = TEXT("RoadPainting_Draw");
const FString URoadPaintingEditorMode::SelectToolName = TEXT("RoadPainting_Select");

URoadPaintingEditorMode::URoadPaintingEditorMode()
{
	FModuleManager::Get().LoadModule(TEXT("EditorStyle"));
	Info = FEditorModeInfo(
		ModeId,
		LOCTEXT("ModeName", "Road Painting"),
		FSlateIcon(),
		true);
}

void URoadPaintingEditorMode::Enter()
{
	UEdMode::Enter();
	const FRoadPaintingEditorModeCommands& Commands = FRoadPaintingEditorModeCommands::Get();
	RegisterTool(Commands.DrawRoad, DrawToolName, NewObject<URoadPaintToolBuilder>(this));
	RegisterTool(Commands.SelectRoad, SelectToolName, NewObject<URoadSelectToolBuilder>(this));

	Toolkit->GetToolkitCommands()->MapAction(
		Commands.DeleteSelection,
		FExecuteAction::CreateUObject(this, &URoadPaintingEditorMode::DeleteSelection));
	Toolkit->GetToolkitCommands()->MapAction(
		Commands.AdoptSelectedRoads,
		FExecuteAction::CreateUObject(this, &URoadPaintingEditorMode::AdoptSelectedRoads));
	Toolkit->GetToolkitCommands()->MapAction(
		Commands.RebuildDirty,
		FExecuteAction::CreateUObject(this, &URoadPaintingEditorMode::RebuildDirty));
	Toolkit->GetToolkitCommands()->MapAction(
		Commands.RebuildAll,
		FExecuteAction::CreateUObject(this, &URoadPaintingEditorMode::RebuildAll));
	Toolkit->GetToolkitCommands()->MapAction(
		Commands.Validate,
		FExecuteAction::CreateUObject(this, &URoadPaintingEditorMode::ValidateNetwork));
	GetToolManager()->SelectActiveToolType(EToolSide::Left, DrawToolName);
}

void URoadPaintingEditorMode::CreateToolkit()
{
	Toolkit = MakeShareable(new FRoadPaintingEditorModeToolkit);
}

TMap<FName, TArray<TSharedPtr<FUICommandInfo>>>
URoadPaintingEditorMode::GetModeCommands() const
{
	return FRoadPaintingEditorModeCommands::GetCommands();
}

ARoadNetworkActor* URoadPaintingEditorMode::FindRoadNetwork() const
{
	if (!GetWorld())
	{
		return nullptr;
	}
	if (GEditor)
	{
		for (FSelectionIterator Iterator(*GEditor->GetSelectedActors()); Iterator; ++Iterator)
		{
			if (ARoadNetworkActor* Network = Cast<ARoadNetworkActor>(*Iterator))
			{
				return Network;
			}
		}
	}
	for (TActorIterator<ARoadNetworkActor> Iterator(GetWorld()); Iterator; ++Iterator)
	{
		return *Iterator;
	}
	return nullptr;
}

ARoadNetworkActor* URoadPaintingEditorMode::GetOrCreateRoadNetwork()
{
	if (ARoadNetworkActor* ExistingNetwork = FindRoadNetwork())
	{
		return ExistingNetwork;
	}
	if (!GetWorld())
	{
		return nullptr;
	}
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.ObjectFlags |= RF_Transactional;
	ARoadNetworkActor* Network = GetWorld()->SpawnActor<ARoadNetworkActor>(
		ARoadNetworkActor::StaticClass(),
		FVector::ZeroVector,
		FRotator::ZeroRotator,
		SpawnParameters);
	if (Network)
	{
		Network->SetActorLabel(TEXT("Road Network"));
	}
	return Network;
}

void URoadPaintingEditorMode::DeleteSelection()
{
	if (ARoadNetworkActor* Network = FindRoadNetwork())
	{
		const FScopedTransaction Transaction(LOCTEXT("DeleteRoadSelection", "Delete Road Network Selection"));
		Network->DeleteSelection();
	}
}

void URoadPaintingEditorMode::AdoptSelectedRoads()
{
	const FScopedTransaction Transaction(LOCTEXT("AdoptRoads", "Adopt Selected Roads"));
	if (ARoadNetworkActor* Network = GetOrCreateRoadNetwork())
	{
		Network->AdoptSelectedRoads();
	}
}

void URoadPaintingEditorMode::RebuildDirty()
{
	if (ARoadNetworkActor* Network = FindRoadNetwork())
	{
		const FScopedTransaction Transaction(LOCTEXT("RebuildDirtyRoads", "Rebuild Dirty Roads"));
		Network->Modify();
		Network->RebuildDirty();
	}
}

void URoadPaintingEditorMode::RebuildAll()
{
	if (ARoadNetworkActor* Network = FindRoadNetwork())
	{
		const FScopedTransaction Transaction(LOCTEXT("RebuildAllRoads", "Rebuild All Roads"));
		Network->Modify();
		Network->RebuildAll();
	}
}

void URoadPaintingEditorMode::ValidateNetwork()
{
	if (ARoadNetworkActor* Network = FindRoadNetwork())
	{
		Network->ValidateNetwork();
	}
}

#undef LOCTEXT_NAMESPACE
