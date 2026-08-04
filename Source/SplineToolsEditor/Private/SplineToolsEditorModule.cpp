#include "SplineToolsEditorModule.h"

#include "RoadPaintingEditorModeCommands.h"

void FSplineToolsEditorModule::StartupModule()
{
	FRoadPaintingEditorModeCommands::Register();
}

void FSplineToolsEditorModule::ShutdownModule()
{
	FRoadPaintingEditorModeCommands::Unregister();
}

IMPLEMENT_MODULE(FSplineToolsEditorModule, SplineToolsEditor)
