// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Containers/Ticker.h"
#include "Misc/Guid.h"
#include "Channels/MovieSceneChannelTraits.h"

class FToolBarBuilder;
class FMenuBuilder;
class SCameraRecorderWidget;
class ULevelSequence;
class ACineCameraActor;
class ISequencer;

/** Keyframe interpolation modes */
enum class ECameraRecorderInterpMode : uint8
{
	Auto,		// Automatic tangent calculation
	User,		// User-defined tangents
	Break,		// Broken tangents (independent in/out)
	Linear,		// Linear interpolation
	Constant	// Step/constant interpolation
};

/** Stores a single recorded camera frame with unwrapped rotation values */
struct FRecordedCameraFrame
{
	int32 FrameNumber;
	FVector Location;
	FRotator Rotation;	// Unwrapped euler angles (can exceed ±180° to prevent snapping)
	FVector Scale;

	FRecordedCameraFrame()
		: FrameNumber(0)
		, Location(FVector::ZeroVector)
		, Rotation(FRotator::ZeroRotator)
		, Scale(FVector::OneVector)
	{}

	FRecordedCameraFrame(int32 InFrame, const FTransform& InTransform)
		: FrameNumber(InFrame)
		, Location(InTransform.GetLocation())
		, Rotation(InTransform.Rotator())
		, Scale(InTransform.GetScale3D())
	{}
};

class FCameraRecorderModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	void PluginButtonClicked();

	// Recording control
	void SetRecording(bool bInIsRecording);
	bool IsRecording() const { return bIsRecording; }

	// Frame configuration
	void SetFrameStep(int32 InFrameStep) { FrameStep = FMath::Max(1, InFrameStep); }
	int32 GetFrameStep() const { return FrameStep; }

	void SetStartFrame(int32 InStartFrame) { StartFrame = FMath::Max(0, InStartFrame); }
	void SetEndFrame(int32 InEndFrame) { EndFrame = FMath::Max(0, InEndFrame); }
	int32 GetStartFrame() const { return StartFrame; }
	int32 GetEndFrame() const { return EndFrame; }
	int32 GetCurrentFrame() const { return CurrentFrame; }

	// Keyframe settings
	void SetInterpMode(ECameraRecorderInterpMode InMode) { InterpMode = InMode; }
	ECameraRecorderInterpMode GetInterpMode() const { return InterpMode; }

	void SetKeyframeOnLastFrame(bool bInKeyframeOnLastFrame) { bKeyframeOnLastFrame = bInKeyframeOnLastFrame; }
	bool GetKeyframeOnLastFrame() const { return bKeyframeOnLastFrame; }

	void SetSnapRotationCorrection(bool bInSnapRotationCorrection) { bSnapRotationCorrection = bInSnapRotationCorrection; }
	bool GetSnapRotationCorrection() const { return bSnapRotationCorrection; }

	// Countdown state queries
	bool IsInCountdown() const { return bIsInCountdown; }
	int32 GetCountdownSeconds() const { return CountdownSecondsRemaining; }
	bool IsWaitingForClick() const { return bWaitingForViewportClick; }

private:
	void RegisterMenus();
	TSharedRef<class SDockTab> OnSpawnPluginTab(const class FSpawnTabArgs& SpawnTabArgs);
	
	// Ticker and recording loop
	bool HandleTicker(float DeltaTime);
	void OnTick();
	void StopRecording();
	void StartSequencerPlayback();
	
	// Countdown management
	void StartCountdown();
	void UpdateCountdown(float DeltaTime);

	// Sequencer interaction
	TSharedPtr<ISequencer> GetActiveSequencer();
	ULevelSequence* GetOrCreateLevelSequence();
	FGuid GetOrCreateCameraBinding(ACineCameraActor* CineCam);
	void ClearExistingKeyframes(const FGuid& CameraBinding);

	// Keyframe recording
	void RecordCameraKeyframe(ACineCameraActor* CineCam, int32 FrameNumber);	// Deprecated
	void RecordCameraKeyframeWithRotation(const FVector& Location, const FRotator& Rotation, int32 FrameNumber);
	void ApplyRotationSnapCorrection();

	// Recording state
	bool bIsRecording = false;
	int32 FrameStep = 1;
	int32 StartFrame = 0;
	int32 EndFrame = 120;
	int32 CurrentFrame = 0;
	int32 LastRecordedFrame = -1;
	ECameraRecorderInterpMode InterpMode = ECameraRecorderInterpMode::Auto;
	bool bKeyframeOnLastFrame = true;
	bool bSnapRotationCorrection = true;
	
	// Countdown state
	bool bWaitingForViewportClick = false;
	bool bIsInCountdown = false;
	float CountdownTimer = 0.0f;
	int32 CountdownSecondsRemaining = 3;
	TOptional<FTransform> CapturedCameraTransform;
	
	// UI and editor references
	TSharedPtr<class FUICommandList> PluginCommands;
	TWeakPtr<SCameraRecorderWidget> CameraRecorderWidget;
	FTSTicker::FDelegateHandle TickDelegateHandle;
	
	// Sequencer and camera tracking
	TWeakObjectPtr<ULevelSequence> CurrentLevelSequence;
	TWeakObjectPtr<ACineCameraActor> RecordingCamera;
	TWeakPtr<ISequencer> ActiveSequencer;
	FGuid CachedCameraBinding;
	
	// Frame buffer for batch writing keyframes
	TArray<FRecordedCameraFrame> RecordedFrames;
};