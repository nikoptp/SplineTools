#include "RoadPaintingEditorMode.h"

#include "Editor.h"
#include "EditorModeManager.h"
#include "EngineUtils.h"
#include "Engine/Selection.h"
#include "InteractiveToolManager.h"
#include "LevelEditorViewport.h"
#include "ProceduralRoadJunctionActor.h"
#include "ProceduralRoadActor.h"
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
		FExecuteAction::CreateUObject(this, &URoadPaintingEditorMode::DeleteSelection),
		FCanExecuteAction::CreateUObject(this, &URoadPaintingEditorMode::CanDeleteSelection));
	Toolkit->GetToolkitCommands()->MapAction(
		Commands.AdoptSelectedRoads,
		FExecuteAction::CreateUObject(this, &URoadPaintingEditorMode::AdoptSelectedRoads),
		FCanExecuteAction::CreateUObject(this, &URoadPaintingEditorMode::CanAdoptSelectedRoads));
	Toolkit->GetToolkitCommands()->MapAction(
		Commands.RebuildDirty,
		FExecuteAction::CreateUObject(this, &URoadPaintingEditorMode::RebuildDirty),
		FCanExecuteAction::CreateUObject(this, &URoadPaintingEditorMode::CanRebuild));
	Toolkit->GetToolkitCommands()->MapAction(
		Commands.RebuildAll,
		FExecuteAction::CreateUObject(this, &URoadPaintingEditorMode::RebuildAll),
		FCanExecuteAction::CreateUObject(this, &URoadPaintingEditorMode::CanRebuild));
	Toolkit->GetToolkitCommands()->MapAction(
		Commands.RebuildLandscapePaint,
		FExecuteAction::CreateUObject(this, &URoadPaintingEditorMode::RebuildLandscapePaint),
		FCanExecuteAction::CreateUObject(this, &URoadPaintingEditorMode::CanRebuild));
	Toolkit->GetToolkitCommands()->MapAction(
		Commands.Validate,
		FExecuteAction::CreateUObject(this, &URoadPaintingEditorMode::ValidateNetwork),
		FCanExecuteAction::CreateUObject(this, &URoadPaintingEditorMode::CanRebuild));
	Toolkit->GetToolkitCommands()->MapAction(
		Commands.FrameNetwork,
		FExecuteAction::CreateUObject(this, &URoadPaintingEditorMode::FrameNetwork),
		FCanExecuteAction::CreateUObject(this, &URoadPaintingEditorMode::CanFrameNetwork));
	Toolkit->GetToolkitCommands()->MapAction(
		Commands.InsertPoint,
		FExecuteAction::CreateUObject(this, &URoadPaintingEditorMode::InsertPoint),
		FCanExecuteAction::CreateUObject(this, &URoadPaintingEditorMode::CanInsertPoint));
	Toolkit->GetToolkitCommands()->MapAction(
		Commands.CancelInteraction,
		FExecuteAction::CreateUObject(this, &URoadPaintingEditorMode::CancelInteraction));
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
		LastOperationText = Network->DeleteSelection()
			? LOCTEXT("DeletedSelection", "Deleted the selected road elements.")
			: LOCTEXT("DeleteSelectionFailed", "Nothing was selected.");
	}
}

void URoadPaintingEditorMode::AdoptSelectedRoads()
{
	if (!CanAdoptSelectedRoads())
	{
		LastOperationText = LOCTEXT("NoRoadsToAdopt", "Select at least one procedural road to adopt.");
		return;
	}
	const FScopedTransaction Transaction(LOCTEXT("AdoptRoads", "Adopt Selected Roads"));
	if (ARoadNetworkActor* Network = GetOrCreateRoadNetwork())
	{
		LastOperationText = Network->AdoptSelectedRoads()
			? LOCTEXT("AdoptedRoads", "Selected roads were adopted by the network.")
			: LOCTEXT("AdoptRoadsFailed", "Selected roads could not be adopted.");
	}
}

void URoadPaintingEditorMode::RebuildDirty()
{
	if (ARoadNetworkActor* Network = FindRoadNetwork())
	{
		const FScopedTransaction Transaction(LOCTEXT("RebuildDirtyRoads", "Rebuild Dirty Roads"));
		Network->Modify();
		Network->RebuildDirty();
		LastOperationText = LOCTEXT("RebuiltDirtyRoads", "Dirty road geometry was rebuilt.");
	}
}

void URoadPaintingEditorMode::RebuildAll()
{
	if (ARoadNetworkActor* Network = FindRoadNetwork())
	{
		const FScopedTransaction Transaction(LOCTEXT("RebuildAllRoads", "Rebuild All Roads"));
		Network->Modify();
		Network->RebuildAll();
		LastOperationText = LOCTEXT("RebuiltAllRoads", "The complete road network was rebuilt.");
	}
}

void URoadPaintingEditorMode::RebuildLandscapePaint()
{
	if (ARoadNetworkActor* Network = FindRoadNetwork())
	{
		const FScopedTransaction Transaction(LOCTEXT("RebuildRoadLandscapePaint", "Rebuild Road Landscape Paint"));
		Network->Modify();
		Network->RebuildLandscapePaint();
		LastOperationText = LOCTEXT("RebuiltRoadLandscapePaint", "Road Landscape paint is rebuilding from the current network.");
	}
}

void URoadPaintingEditorMode::ValidateNetwork()
{
	if (ARoadNetworkActor* Network = FindRoadNetwork())
	{
		FString Errors;
		if (Network->ValidateNetworkGraph(Errors))
		{
			LastValidationText = LOCTEXT("ValidationPassed", "Validation passed: the road network is valid.");
			LastOperationText = LastValidationText;
		}
		else
		{
			LastValidationText = FText::Format(
				LOCTEXT("ValidationFailed", "Validation found problems:\n{0}"),
				FText::FromString(Errors));
			LastOperationText = LastValidationText;
		}
	}
}

bool URoadPaintingEditorMode::CanDeleteSelection() const
{
	if (ARoadNetworkActor* Network = FindRoadNetwork())
	{
		return !Network->GetSelectedPointIds().IsEmpty()
			|| !Network->GetSelectedLinkIds().IsEmpty();
	}
	return false;
}

bool URoadPaintingEditorMode::CanAdoptSelectedRoads() const
{
	if (!GEditor)
	{
		return false;
	}
	for (FSelectionIterator Iterator(*GEditor->GetSelectedActors()); Iterator; ++Iterator)
	{
		if (Cast<AProceduralRoadActor>(*Iterator)
			|| Cast<AProceduralRoadJunctionActor>(*Iterator))
		{
			return true;
		}
	}
	return false;
}

bool URoadPaintingEditorMode::CanRebuild() const
{
	return FindRoadNetwork() != nullptr;
}

bool URoadPaintingEditorMode::CanFrameNetwork() const
{
	if (ARoadNetworkActor* Network = FindRoadNetwork())
	{
		return !Network->GetPoints().IsEmpty();
	}
	return false;
}

bool URoadPaintingEditorMode::CanInsertPoint() const
{
	if (ARoadNetworkActor* Network = FindRoadNetwork())
	{
		return Network->GetSelectedPointIds().IsEmpty()
			&& Network->GetSelectedLinkIds().Num() == 1;
	}
	return false;
}

void URoadPaintingEditorMode::FrameNetwork()
{
	ARoadNetworkActor* Network = FindRoadNetwork();
	FLevelEditorViewportClient* ViewportClient = GCurrentLevelEditingViewportClient;
	if (!Network || !ViewportClient)
	{
		return;
	}

	FBox Bounds(ForceInit);
	if (!Network->GetSelectedPointIds().IsEmpty()
		|| !Network->GetSelectedLinkIds().IsEmpty())
	{
		for (const FGuid& PointId : Network->GetSelectedPointIds())
		{
			if (const FRoadNetworkPoint* Point = Network->FindPoint(PointId))
			{
				Bounds += Point->WorldLocation;
			}
		}
		for (const FGuid& LinkId : Network->GetSelectedLinkIds())
		{
			if (const FRoadNetworkLink* Link = Network->FindLink(LinkId))
			{
				TArray<FVector> LinkSamples;
				Network->GetLinkWorldSamples(*Link, LinkSamples);
				for (const FVector& Point : LinkSamples)
				{
					Bounds += Point;
				}
			}
		}
	}
	else
	{
		for (const FRoadNetworkPoint& Point : Network->GetPoints())
		{
			Bounds += Point.WorldLocation;
		}
	}

	if (Bounds.IsValid)
	{
		Bounds = Bounds.ExpandBy(1000.0f);
		ViewportClient->FocusViewportOnBox(Bounds);
		LastOperationText = LOCTEXT("FramedNetwork", "Viewport framed the current road selection.");
	}
}

void URoadPaintingEditorMode::InsertPoint()
{
	if (ARoadNetworkActor* Network = FindRoadNetwork())
	{
		const FScopedTransaction Transaction(LOCTEXT("InsertRoadPoint", "Insert Road Point"));
		Network->Modify();
		LastOperationText = Network->InsertPointOnLink(Network->GetSelectedLinkId())
			? LOCTEXT("InsertedRoadPoint", "Inserted and selected a road control point.")
			: LOCTEXT("InsertRoadPointFailed", "The selected link could not be split.");
	}
}

void URoadPaintingEditorMode::CancelInteraction()
{
	if (GetToolManager())
	{
		GetToolManager()->DeactivateTool(EToolSide::Left, EToolShutdownType::Cancel);
		LastOperationText = LOCTEXT("CancelledInteraction", "Road editing interaction cancelled.");
	}
}

FText URoadPaintingEditorMode::GetNetworkNameText() const
{
	if (ARoadNetworkActor* Network = FindRoadNetwork())
	{
		return FText::Format(
			LOCTEXT("NetworkName", "Network: {0}"),
			FText::FromString(Network->GetActorLabel()));
	}
	return LOCTEXT("NoNetworkName", "Network: None");
}

FText URoadPaintingEditorMode::GetNetworkSummaryText() const
{
	if (ARoadNetworkActor* Network = FindRoadNetwork())
	{
		return FText::Format(
			LOCTEXT("NetworkSummary", "Points {0}   Links {1}   Junctions {2}"),
			Network->GetPoints().Num(),
			Network->GetLinks().Num(),
			Network->GetGeneratedJunctionCount());
	}
	return LOCTEXT("NoNetworkSummary", "No road network has been created.");
}

FText URoadPaintingEditorMode::GetSelectionText() const
{
	if (ARoadNetworkActor* Network = FindRoadNetwork())
	{
		const int32 PointCount = Network->GetSelectedPointIds().Num();
		const int32 LinkCount = Network->GetSelectedLinkIds().Num();
		if (PointCount == 1 && LinkCount == 0)
		{
			return LOCTEXT("SelectedPoint", "Selected: control point");
		}
		if (LinkCount == 1 && PointCount == 0)
		{
			return LOCTEXT("SelectedLink", "Selected: road link");
		}
		if (PointCount > 0 || LinkCount > 0)
		{
			return FText::Format(
				LOCTEXT("SelectedElements", "Selected: {0} points, {1} links"),
				FText::AsNumber(PointCount),
				FText::AsNumber(LinkCount));
		}
	}
	return LOCTEXT("NoSelection", "Selected: none");
}

FText URoadPaintingEditorMode::GetNetworkStatusText() const
{
	if (!FindRoadNetwork())
	{
		return LOCTEXT("ReadyWithoutNetwork", "No road network found. Draw to create one.");
	}
	if (ARoadNetworkActor* Network = FindRoadNetwork())
	{
		if (Network->HasPendingRebuild())
		{
			return FText::Format(
				LOCTEXT("NetworkNeedsRebuild", "{0}  |  Rebuild pending"),
				GetSelectionText());
		}
		return FText::Format(
			LOCTEXT("NetworkReadyWithPaintStatus", "{0}  |  Ready\n{1}"),
			GetSelectionText(),
			Network->GetLandscapePaintStatusText());
	}
	return FText::Format(LOCTEXT("NetworkReady", "{0}  |  Ready"), GetSelectionText());
}

FText URoadPaintingEditorMode::GetLastOperationText() const
{
	return LastOperationText.IsEmpty()
		? LOCTEXT("NoRecentOperation", "Tip: Draw a road over the landscape, then release to commit it.")
		: LastOperationText;
}

int32 URoadPaintingEditorMode::GetNetworkPointCount() const
{
	if (const ARoadNetworkActor* Network = FindRoadNetwork())
	{
		return Network->GetPoints().Num();
	}
	return 0;
}

int32 URoadPaintingEditorMode::GetNetworkLinkCount() const
{
	if (const ARoadNetworkActor* Network = FindRoadNetwork())
	{
		return Network->GetLinks().Num();
	}
	return 0;
}

int32 URoadPaintingEditorMode::GetNetworkJunctionCount() const
{
	if (const ARoadNetworkActor* Network = FindRoadNetwork())
	{
		return Network->GetGeneratedJunctionCount();
	}
	return 0;
}

bool URoadPaintingEditorMode::HasNetwork() const
{
	return FindRoadNetwork() != nullptr;
}

bool URoadPaintingEditorMode::IsNetworkDirty() const
{
	if (const ARoadNetworkActor* Network = FindRoadNetwork())
	{
		return Network->HasPendingRebuild();
	}
	return false;
}

bool URoadPaintingEditorMode::HasSelection() const
{
	return CanDeleteSelection();
}

FText URoadPaintingEditorMode::GetSelectedElementText() const
{
	return GetSelectionText();
}

FText URoadPaintingEditorMode::GetValidationResultText() const
{
	return LastValidationText.IsEmpty()
		? LOCTEXT("NoValidationResult", "Validation has not been run.")
		: LastValidationText;
}

#undef LOCTEXT_NAMESPACE
