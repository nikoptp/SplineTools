#include "RoadPaintingEditorModeToolkit.h"

#define LOCTEXT_NAMESPACE "RoadPaintingEditorModeToolkit"

void FRoadPaintingEditorModeToolkit::Init(
	const TSharedPtr<IToolkitHost>& InitToolkitHost,
	TWeakObjectPtr<UEdMode> InOwningMode)
{
	FModeToolkit::Init(InitToolkitHost, InOwningMode);
}

void FRoadPaintingEditorModeToolkit::GetToolPaletteNames(
	TArray<FName>& PaletteNames) const
{
	PaletteNames.Add(NAME_Default);
}

FName FRoadPaintingEditorModeToolkit::GetToolkitFName() const
{
	return FName(TEXT("RoadPaintingEditorMode"));
}

FText FRoadPaintingEditorModeToolkit::GetBaseToolkitName() const
{
	return LOCTEXT("DisplayName", "Road Painting");
}

#undef LOCTEXT_NAMESPACE
