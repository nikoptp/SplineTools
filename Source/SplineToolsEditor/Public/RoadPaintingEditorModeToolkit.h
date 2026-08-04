#pragma once

#include "Toolkits/BaseToolkit.h"

class FRoadPaintingEditorModeToolkit : public FModeToolkit
{
public:
	virtual void Init(
		const TSharedPtr<IToolkitHost>& InitToolkitHost,
		TWeakObjectPtr<UEdMode> InOwningMode) override;
	virtual void GetToolPaletteNames(TArray<FName>& PaletteNames) const override;
	virtual FName GetToolkitFName() const override;
	virtual FText GetBaseToolkitName() const override;
};
