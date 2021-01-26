// Created by Kaan Buran
// See https://github.com/Naocum/UE4VoiceChat for documentation and licensing

#include "UE4VoiceChatModule.h"
#include "Core.h"
#include "Modules/ModuleManager.h"
//#include "Interfaces/IPluginManager.h"

//#include "SimulationPluginLibrary/ExampleLibrary.h"

#define LOCTEXT_NAMESPACE "FUE4VoiceChat"

void FUE4VoiceChat::StartupModule()
{
	
}

void FUE4VoiceChat::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.

}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FUE4VoiceChat, UE4VoiceChat)
