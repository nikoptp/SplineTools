#pragma once

#include "BaseBehaviors/ClickDragBehavior.h"
#include "InteractiveToolBuilder.h"
#include "ProceduralRoadActor.h"
#include "RoadPaintTool.generated.h"

class AProceduralRoadJunctionActor;
class ARoadNetworkActor;
class URoadPaintingEditorMode;

UCLASS()
class SPLINETOOLSEDITOR_API URoadPaintToolBuilder : public UInteractiveToolBuilder
{
	GENERATED_BODY()

public:
	virtual bool CanBuildTool(const FToolBuilderState& SceneState) const override;
	virtual UInteractiveTool* BuildTool(const FToolBuilderState& SceneState) const override;
};

UCLASS(Transient)
class SPLINETOOLSEDITOR_API URoadPaintToolProperties : public UInteractiveToolPropertySet
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Classes")
	TSubclassOf<AProceduralRoadActor> RoadClass;

	UPROPERTY(EditAnywhere, Category = "Classes")
	TSubclassOf<AProceduralRoadJunctionActor> JunctionClass;

	UPROPERTY(EditAnywhere, Category = "Brush", meta = (ClampMin = "10.0"))
	float SampleSpacing = 200.0f;

	UPROPERTY(EditAnywhere, Category = "Brush", meta = (ClampMin = "0.0"))
	float SimplificationTolerance = 100.0f;

	UPROPERTY(EditAnywhere, Category = "Connections", meta = (ClampMin = "1.0"))
	float SnapRadius = 300.0f;

	UPROPERTY(EditAnywhere, Category = "Connections", meta = (ClampMin = "10.0"))
	float CrossingSampleInterval = 100.0f;

	UPROPERTY(EditAnywhere, Category = "Connections", meta = (ClampMin = "1.0"))
	float IntersectionMergeRadius = 100.0f;

	UPROPERTY(EditAnywhere, Category = "Connections", meta = (ClampMin = "0.0"))
	float MaximumJunctionHeightDifference = 200.0f;
};

UCLASS()
class SPLINETOOLSEDITOR_API URoadPaintTool
	: public UInteractiveTool
	, public IClickDragBehaviorTarget
{
	GENERATED_BODY()

public:
	void Initialize(UWorld* World, URoadPaintingEditorMode* EditorMode);

	virtual void Setup() override;
	virtual void Shutdown(EToolShutdownType ShutdownType) override;
	virtual void Render(IToolsContextRenderAPI* RenderAPI) override;
	virtual FInputRayHit CanBeginClickDragSequence(const FInputDeviceRay& PressPos) override;
	virtual void OnClickPress(const FInputDeviceRay& PressPos) override;
	virtual void OnClickDrag(const FInputDeviceRay& DragPos) override;
	virtual void OnClickRelease(const FInputDeviceRay& ReleasePos) override;
	virtual void OnTerminateDragSequence() override;
	virtual void OnUpdateModifierState(int ModifierID, bool bIsOn) override;

private:
	FInputRayHit FindLandscapeHit(const FRay& WorldRay, FVector& OutLocation) const;
	void AddStrokeSample(const FVector& WorldLocation, bool bForce);
	void UpdateStrokePreview();
	ARoadNetworkActor* FindNetwork() const;
	void CopyPropertiesToNetwork(ARoadNetworkActor* Network) const;

	UPROPERTY()
	TObjectPtr<URoadPaintToolProperties> Properties;

	UPROPERTY()
	TObjectPtr<URoadPaintingEditorMode> Mode;

	UPROPERTY()
	TObjectPtr<UWorld> TargetWorld;

	TArray<FVector> StrokePoints;
	TArray<FVector> SimplifiedStrokePoints;
	TArray<FVector> PreviewIntersections;
	FVector CursorLocation = FVector::ZeroVector;
	FVector SnapTargetLocation = FVector::ZeroVector;
	bool bHasCursor = false;
	bool bHasSnapTarget = false;
	bool bDragging = false;
};
