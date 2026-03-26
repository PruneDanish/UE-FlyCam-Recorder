// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Containers/Ticker.h"

class FToolBarBuilder;
class FMenuBuilder;
class SCameraRecorderWidget;
class ULevelSequence;
class ACineCameraActor;

class FCameraRecorderModule : public IModuleInterface
{
public:

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	void PluginButtonClicked();

	/** Called by widget to set recording state */
	void SetRecording(bool bInIsRecording);
	bool IsRecording() const { return bIsRecording; }

	/** Frame stepping */
	void SetFrameStep(int32 InFrameStep) { FrameStep = FMath::Max(1, InFrameStep); }
	int32 GetFrameStep() const { return FrameStep; }

	/** Frame range */
	void SetStartFrame(int32 InStartFrame) { StartFrame = FMath::Max(0, InStartFrame); }
	void SetEndFrame(int32 InEndFrame) { EndFrame = FMath::Max(0, InEndFrame); }
	int32 GetStartFrame() const { return StartFrame; }
	int32 GetEndFrame() const { return EndFrame; }
	int32 GetCurrentFrame() const { return CurrentFrame; }

private:

	void RegisterMenus();
	TSharedRef<class SDockTab> OnSpawnPluginTab(const class FSpawnTabArgs& SpawnTabArgs);
	
	bool HandleTicker(float DeltaTime);
	void OnTick();
	void StopRecording();

	void RecordCameraKeyframe(ACineCameraActor* CineCam, int32 FrameNumber);
	ULevelSequence* GetOrCreateLevelSequence();

	bool bIsRecording = false;
	int32 FrameStep = 1;
	int32 StartFrame = 0;
	int32 EndFrame = 120;
	int32 CurrentFrame = 0;
	
	TSharedPtr<class FUICommandList> PluginCommands;
	TWeakPtr<SCameraRecorderWidget> CameraRecorderWidget;
	FTSTicker::FDelegateHandle TickDelegateHandle;
	
	TWeakObjectPtr<ULevelSequence> CurrentLevelSequence;
	TWeakObjectPtr<ACineCameraActor> RecordingCamera;
};