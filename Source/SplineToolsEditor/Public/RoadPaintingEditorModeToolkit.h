#pragma once

#include "Toolkits/BaseToolkit.h"

class SWidget;
class URoadPaintingEditorMode;

class FRoadPaintingEditorModeToolkit : public FModeToolkit
{
public:
	virtual void Init(
		const TSharedPtr<IToolkitHost>& InitToolkitHost,
		TWeakObjectPtr<UEdMode> InOwningMode) override;
	virtual void GetToolPaletteNames(TArray<FName>& PaletteNames) const override;
	virtual void BuildToolPalette(FName Palette, FToolBarBuilder& ToolbarBuilder) override;
	virtual TSharedPtr<SWidget> GetInlineContent() const override;
	virtual FName GetToolkitFName() const override;
	virtual FText GetBaseToolkitName() const override;

private:
	TWeakObjectPtr<URoadPaintingEditorMode> Mode;
	mutable TSharedPtr<SWidget> InlineContent;
};
