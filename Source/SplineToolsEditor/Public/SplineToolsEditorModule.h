#pragma once

#include "Modules/ModuleManager.h"

class FSplineToolsEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
