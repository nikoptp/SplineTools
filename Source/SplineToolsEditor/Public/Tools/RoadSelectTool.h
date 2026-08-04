#pragma once

#include "BaseBehaviors/ClickDragBehavior.h"
#include "InteractiveToolBuilder.h"
#include "RoadSelectTool.generated.h"

class ARoadNetworkActor;
class FScopedTransaction;
class URoadPaintingEditorMode;

UCLASS()
class SPLINETOOLSEDITOR_API URoadSelectToolBuilder : public UInteractiveToolBuilder
{
	GENERATED_BODY()

public:
	virtual bool CanBuildTool(const FToolBuilderState& SceneState) const override;
	virtual UInteractiveTool* BuildTool(const FToolBuilderState& SceneState) const override;
};

UCLASS(Transient)
class SPLINETOOLSEDITOR_API URoadSelectToolProperties : public UInteractiveToolPropertySet
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Selection", meta = (ClampMin = "10.0"))
	float SelectionRadius = 200.0f;
};

UCLASS()
class SPLINETOOLSEDITOR_API URoadSelectTool
	: public UInteractiveTool
	, public IClickDragBehaviorTarget
{
	GENERATED_BODY()

public:
	void Initialize(UWorld* World, URoadPaintingEditorMode* EditorMode);

	virtual void Setup() override;
	virtual void Render(IToolsContextRenderAPI* RenderAPI) override;
	virtual FInputRayHit CanBeginClickDragSequence(const FInputDeviceRay& PressPos) override;
	virtual void OnClickPress(const FInputDeviceRay& PressPos) override;
	virtual void OnClickDrag(const FInputDeviceRay& DragPos) override;
	virtual void OnClickRelease(const FInputDeviceRay& ReleasePos) override;
	virtual void OnTerminateDragSequence() override;
	virtual void OnUpdateModifierState(int ModifierID, bool bIsOn) override;

private:
	FInputRayHit FindLandscapeHit(const FRay& WorldRay, FVector& OutLocation) const;
	ARoadNetworkActor* FindNetwork() const;
	void SelectAtLocation(const FVector& WorldLocation);

	UPROPERTY()
	TObjectPtr<URoadSelectToolProperties> Properties;

	UPROPERTY()
	TObjectPtr<URoadPaintingEditorMode> Mode;

	UPROPERTY()
	TObjectPtr<UWorld> TargetWorld;

	TUniquePtr<FScopedTransaction> MoveTransaction;
	bool bMovingPoint = false;
};
