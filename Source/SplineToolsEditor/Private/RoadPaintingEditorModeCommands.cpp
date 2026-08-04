#include "RoadPaintingEditorModeCommands.h"

#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "RoadPaintingEditorModeCommands"

FRoadPaintingEditorModeCommands::FRoadPaintingEditorModeCommands()
	: TCommands<FRoadPaintingEditorModeCommands>(
		TEXT("RoadPaintingEditorMode"),
		LOCTEXT("ContextName", "Road Painting"),
		NAME_None,
		FAppStyle::GetAppStyleSetName())
{
}

void FRoadPaintingEditorModeCommands::RegisterCommands()
{
	TArray<TSharedPtr<FUICommandInfo>>& ToolCommands = Commands.FindOrAdd(NAME_Default);
	UI_COMMAND(DrawRoad, "Draw", "Paint an ordered road stroke on the landscape", EUserInterfaceActionType::ToggleButton, FInputChord());
	UI_COMMAND(SelectRoad, "Select / Move", "Select and move road network points or links", EUserInterfaceActionType::ToggleButton, FInputChord());
	UI_COMMAND(DeleteSelection, "Delete", "Delete the selected road point or link", EUserInterfaceActionType::Button, FInputChord(EKeys::Delete));
	UI_COMMAND(AdoptSelectedRoads, "Adopt Selected", "Import selected procedural roads and junctions", EUserInterfaceActionType::Button, FInputChord());
	UI_COMMAND(RebuildDirty, "Rebuild Dirty", "Rebuild affected managed roads and refresh all managed junction caches", EUserInterfaceActionType::Button, FInputChord());
	UI_COMMAND(RebuildAll, "Rebuild All", "Regenerate the complete managed road network", EUserInterfaceActionType::Button, FInputChord());
	UI_COMMAND(Validate, "Validate", "Validate graph identities, links, and classes", EUserInterfaceActionType::Button, FInputChord());
	ToolCommands.Add(DrawRoad);
	ToolCommands.Add(SelectRoad);
	ToolCommands.Add(DeleteSelection);
	ToolCommands.Add(AdoptSelectedRoads);
	ToolCommands.Add(RebuildDirty);
	ToolCommands.Add(RebuildAll);
	ToolCommands.Add(Validate);
}

TMap<FName, TArray<TSharedPtr<FUICommandInfo>>>
FRoadPaintingEditorModeCommands::GetCommands()
{
	return Get().Commands;
}

#undef LOCTEXT_NAMESPACE
