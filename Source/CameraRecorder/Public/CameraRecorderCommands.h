// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Framework/Commands/Commands.h"
#include "CameraRecorderStyle.h"

class FCameraRecorderCommands : public TCommands<FCameraRecorderCommands>
{
public:

	FCameraRecorderCommands()
		: TCommands<FCameraRecorderCommands>(TEXT("CameraRecorder"), NSLOCTEXT("Contexts", "CameraRecorder", "CameraRecorder Plugin"), NAME_None, FCameraRecorderStyle::GetStyleSetName())
	{
	}

	// TCommands<> interface
	virtual void RegisterCommands() override;

public:
	TSharedPtr< FUICommandInfo > OpenPluginWindow;
};