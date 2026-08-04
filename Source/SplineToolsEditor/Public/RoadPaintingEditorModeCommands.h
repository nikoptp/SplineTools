#pragma once

#include "Framework/Commands/Commands.h"

class FRoadPaintingEditorModeCommands : public TCommands<FRoadPaintingEditorModeCommands>
{
public:
	FRoadPaintingEditorModeCommands();

	virtual void RegisterCommands() override;
	static TMap<FName, TArray<TSharedPtr<FUICommandInfo>>> GetCommands();

	TSharedPtr<FUICommandInfo> DrawRoad;
	TSharedPtr<FUICommandInfo> SelectRoad;
	TSharedPtr<FUICommandInfo> DeleteSelection;
	TSharedPtr<FUICommandInfo> AdoptSelectedRoads;
	TSharedPtr<FUICommandInfo> RebuildDirty;
	TSharedPtr<FUICommandInfo> RebuildAll;
	TSharedPtr<FUICommandInfo> Validate;

private:
	TMap<FName, TArray<TSharedPtr<FUICommandInfo>>> Commands;
};
