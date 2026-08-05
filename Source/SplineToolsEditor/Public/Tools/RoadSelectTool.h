#pragma once

#include "BaseBehaviors/ClickDragBehavior.h"
#include "BaseBehaviors/MouseHoverBehavior.h"
#include "BaseBehaviors/MouseWheelBehavior.h"
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
	, public IHoverBehaviorTarget
	, public IMouseWheelBehaviorTarget
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
	virtual FInputRayHit BeginHoverSequenceHitTest(const FInputDeviceRay& PressPos) override;
	virtual void OnBeginHover(const FInputDeviceRay& DevicePos) override;
	virtual bool OnUpdateHover(const FInputDeviceRay& DevicePos) override;
	virtual void OnEndHover() override;
	virtual FInputRayHit ShouldRespondToMouseWheel(const FInputDeviceRay& CurrentPos) override;
	virtual void OnMouseWheelScrollUp(const FInputDeviceRay& CurrentPos) override;
	virtual void OnMouseWheelScrollDown(const FInputDeviceRay& CurrentPos) override;

private:
	FInputRayHit FindLandscapeHit(const FRay& WorldRay, FVector& OutLocation) const;
	ARoadNetworkActor* FindNetwork() const;
	void FindClosestElement(
		const FVector& WorldLocation,
		FGuid& OutPointId,
		FGuid& OutLinkId) const;
	void SelectAtLocation(const FVector& WorldLocation);
	void SelectInScreenRect(const FBox2D& ScreenRect);
	bool ProjectWorldToScreen(
		const FVector& WorldLocation,
		FVector2D& OutScreenLocation) const;
	void ResetDragState();

	UPROPERTY()
	TObjectPtr<URoadSelectToolProperties> Properties;

	UPROPERTY()
	TObjectPtr<URoadPaintingEditorMode> Mode;

	UPROPERTY()
	TObjectPtr<UWorld> TargetWorld;

	TUniquePtr<FScopedTransaction> MoveTransaction;
	bool bMovingSelection = false;
	bool bMarqueeSelecting = false;
	bool bMovedSelection = false;
	FVector CursorLocation = FVector::ZeroVector;
	bool bHasCursor = false;
	FGuid HoverPointId;
	FGuid HoverLinkId;
	FVector2D MarqueeStartScreen = FVector2D::ZeroVector;
	FVector2D MarqueeEndScreen = FVector2D::ZeroVector;
	bool bHasMarqueeScreenPosition = false;
	FMatrix CachedViewProjectionMatrix = FMatrix::Identity;
	FIntRect CachedViewRect;
	bool bHasCachedView = false;
};
