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

// NEW: Structure to store transform with unwrapped euler angles
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
	bool SetRecording(bool bInIsRecording);  // Changed to return bool
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

	/** Warmup frames */
	void SetWarmupFrames(int32 InWarmupFrames) { WarmupFrames = FMath::Max(0, InWarmupFrames); }
	int32 GetWarmupFrames() const { return WarmupFrames; }

	/** Interpolation mode */
	void SetInterpMode(ECameraRecorderInterpMode InMode) { InterpMode = InMode; }
	ECameraRecorderInterpMode GetInterpMode() const { return InterpMode; }

	/** Keyframe on last frame */
	void SetKeyframeOnLastFrame(bool bInKeyframeOnLastFrame) { bKeyframeOnLastFrame = bInKeyframeOnLastFrame; }
	bool GetKeyframeOnLastFrame() const { return bKeyframeOnLastFrame; }

	/** Snap rotation correction */
	void SetSnapRotationCorrection(bool bInSnapRotationCorrection) { bSnapRotationCorrection = bInSnapRotationCorrection; }
	bool GetSnapRotationCorrection() const { return bSnapRotationCorrection; }

	/** Get warmup state */
	bool IsInWarmup() const { return bIsInWarmup; }

private:

	void RegisterMenus();
	TSharedRef<class SDockTab> OnSpawnPluginTab(const class FSpawnTabArgs& SpawnTabArgs);
	
	bool HandleTicker(float DeltaTime);
	void OnTick();
	void StopRecording();
	void StartSequencerPlayback();
	TSharedPtr<ISequencer> GetActiveSequencer();

	void RecordCameraKeyframe(ACineCameraActor* CineCam, int32 FrameNumber);
	void RecordCameraKeyframeWithRotation(const FVector& Location, const FRotator& Rotation, int32 FrameNumber); // NEW
	ULevelSequence* GetOrCreateLevelSequence();
	FGuid GetOrCreateCameraBinding(ACineCameraActor* CineCam);
	void ClearExistingKeyframes(const FGuid& CameraBinding);
	void ApplyRotationSnapCorrection(); // NEW: Post-process recorded transforms

	bool bIsRecording = false;
	bool bIsInWarmup = false;
	int32 FrameStep = 1;
	int32 StartFrame = 0;
	int32 EndFrame = 120;
	int32 WarmupFrames = 30;
	int32 CurrentFrame = 0;
	int32 LastRecordedFrame = -1;
	int32 WarmupStartFrame = 0;
	ECameraRecorderInterpMode InterpMode = ECameraRecorderInterpMode::Auto;
	bool bKeyframeOnLastFrame = true;
	bool bSnapRotationCorrection = true; // NEW: Enable by default
	
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