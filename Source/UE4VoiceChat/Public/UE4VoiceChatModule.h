// Created by Kaan Buran
// See https://github.com/Naocum/UE4VoiceChat for documentation and licensing

#pragma once

#include "Modules/ModuleManager.h"

class FUE4VoiceChat : public IModuleInterface
{
public:

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

};
