// Copyright Epic Games, Inc. All Rights Reserved.

#include "CameraRecorderCommands.h"

#define LOCTEXT_NAMESPACE "FCameraRecorderModule"

void FCameraRecorderCommands::RegisterCommands()
{
	UI_COMMAND(OpenPluginWindow, "FlyCam Recorder", "Bring up FlyCam Recorder window", EUserInterfaceActionType::Button, FInputChord());
}

#undef LOCTEXT_NAMESPACE
