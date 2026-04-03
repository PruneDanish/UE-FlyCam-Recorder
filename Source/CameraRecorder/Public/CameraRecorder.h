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

// Animation interpolation modes for keyframes
enum class ECameraRecorderInterpMode : uint8
{
	Auto,
	User,
	Break,
	Linear,
	Constant
};

// Structure to store transform with unwrapped euler angles
struct FRecordedCameraFrame
{
	int32 FrameNumber;
	FVector Location;
	FRotator Rotation;  // Unwrapped euler angles (can exceed ±180°)
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

	/** Interpolation mode */
	void SetInterpMode(ECameraRecorderInterpMode InMode) { InterpMode = InMode; }
	ECameraRecorderInterpMode GetInterpMode() const { return InterpMode; }

	/** Keyframe on last frame */
	void SetKeyframeOnLastFrame(bool bInKeyframeOnLastFrame) { bKeyframeOnLastFrame = bInKeyframeOnLastFrame; }
	bool GetKeyframeOnLastFrame() const { return bKeyframeOnLastFrame; }

	/** Snap rotation correction */
	void SetSnapRotationCorrection(bool bInSnapRotationCorrection) { bSnapRotationCorrection = bInSnapRotationCorrection; }
	bool GetSnapRotationCorrection() const { return bSnapRotationCorrection; }

	/** Get countdown state */
bool IsInCountdown() const { return bIsInCountdown; }
int32 GetCountdownSeconds() const { return CountdownSecondsRemaining; }
bool IsWaitingForClick() const { return bWaitingForViewportClick; }

private:

	void RegisterMenus();
	TSharedRef<class SDockTab> OnSpawnPluginTab(const class FSpawnTabArgs& SpawnTabArgs);
	
	bool HandleTicker(float DeltaTime);
	void OnTick();
	void StopRecording();
	void StartSequencerPlayback();
	TSharedPtr<ISequencer> GetActiveSequencer();
	void StartCountdown();
	void UpdateCountdown(float DeltaTime);

	void RecordCameraKeyframe(ACineCameraActor* CineCam, int32 FrameNumber);
	void RecordCameraKeyframeWithRotation(const FVector& Location, const FRotator& Rotation, int32 FrameNumber);
	ULevelSequence* GetOrCreateLevelSequence();
	FGuid GetOrCreateCameraBinding(ACineCameraActor* CineCam);
	void ClearExistingKeyframes(const FGuid& CameraBinding);
	void ApplyRotationSnapCorrection();

	bool bIsRecording = false;
	int32 FrameStep = 1;
	int32 StartFrame = 0;
	int32 EndFrame = 120;
	int32 CurrentFrame = 0;
	int32 LastRecordedFrame = -1;
	ECameraRecorderInterpMode InterpMode = ECameraRecorderInterpMode::Auto;
	bool bKeyframeOnLastFrame = true;
	bool bSnapRotationCorrection = true;
	
	// Click to start mode (always enabled now)
	bool bWaitingForViewportClick = false;
	bool bIsInCountdown = false;
	float CountdownTimer = 0.0f;
	int32 CountdownSecondsRemaining = 3;
	TOptional<FTransform> CapturedCameraTransform; // Store the captured transform
	
	TSharedPtr<class FUICommandList> PluginCommands;
	TWeakPtr<SCameraRecorderWidget> CameraRecorderWidget;
	FTSTicker::FDelegateHandle TickDelegateHandle;
	
	TWeakObjectPtr<ULevelSequence> CurrentLevelSequence;
	TWeakObjectPtr<ACineCameraActor> RecordingCamera;
	TWeakPtr<ISequencer> ActiveSequencer;
	FGuid CachedCameraBinding;
	
	// Storage for recorded frames with unwrapped rotations
	TArray<FRecordedCameraFrame> RecordedFrames;
};