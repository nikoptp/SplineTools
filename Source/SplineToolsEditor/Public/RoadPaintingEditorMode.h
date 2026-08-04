#pragma once

#include "Tools/UEdMode.h"
#include "RoadPaintingEditorMode.generated.h"

class ARoadNetworkActor;

UCLASS()
class SPLINETOOLSEDITOR_API URoadPaintingEditorMode : public UEdMode
{
	GENERATED_BODY()

public:
	static const FEditorModeID ModeId;
	static const FString DrawToolName;
	static const FString SelectToolName;

	URoadPaintingEditorMode();

	virtual void Enter() override;
	virtual void CreateToolkit() override;
	virtual TMap<FName, TArray<TSharedPtr<FUICommandInfo>>> GetModeCommands() const override;

	ARoadNetworkActor* FindRoadNetwork() const;
	ARoadNetworkActor* GetOrCreateRoadNetwork();

private:
	void DeleteSelection();
	void AdoptSelectedRoads();
	void RebuildDirty();
	void RebuildAll();
	void ValidateNetwork();
};
