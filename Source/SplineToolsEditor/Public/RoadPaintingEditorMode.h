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

	void DeleteSelection();
	void AdoptSelectedRoads();
	void RebuildDirty();
	void RebuildAll();
	void RebuildLandscapePaint();
	void ValidateNetwork();
	void FrameNetwork();
	void InsertPoint();
	void CancelInteraction();

	bool CanDeleteSelection() const;
	bool CanAdoptSelectedRoads() const;
	bool CanRebuild() const;
	bool CanFrameNetwork() const;
	bool CanInsertPoint() const;

	FText GetNetworkNameText() const;
	FText GetNetworkSummaryText() const;
	FText GetNetworkStatusText() const;
	FText GetLastOperationText() const;
	int32 GetNetworkPointCount() const;
	int32 GetNetworkLinkCount() const;
	int32 GetNetworkJunctionCount() const;
	bool HasNetwork() const;
	bool IsNetworkDirty() const;
	bool HasSelection() const;
	FText GetSelectedElementText() const;
	FText GetValidationResultText() const;

private:
	FText GetSelectionText() const;

	FText LastOperationText;
	FText LastValidationText;
};
