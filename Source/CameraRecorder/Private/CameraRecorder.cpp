// Copyright Epic Games, Inc. All Rights Reserved.

#include "CameraRecorder.h"
#include "CameraRecorderStyle.h"
#include "CameraRecorderCommands.h"
#include "SCameraRecorderWidget.h"
#include "LevelEditor.h"
#include "Widgets/Docking/SDockTab.h"
#include "ToolMenus.h"
#include "LevelEditorSubsystem.h"
#include "CineCameraActor.h"
#include "Editor.h"
#include "LevelSequence.h"
#include "ILevelSequenceEditorToolkit.h"
#include "ISequencer.h"
#include "MovieSceneSequence.h"
#include "MovieScene.h"
#include "MovieSceneTrack.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Subsystems/AssetEditorSubsystem.h"

static const FName CameraRecorderTabName("CameraRecorder");

#define LOCTEXT_NAMESPACE "FCameraRecorderModule"

void FCameraRecorderModule::StartupModule()
{
	FCameraRecorderStyle::Initialize();
	FCameraRecorderStyle::ReloadTextures();

	FCameraRecorderCommands::Register();
	
	PluginCommands = MakeShareable(new FUICommandList);

	PluginCommands->MapAction(
		FCameraRecorderCommands::Get().OpenPluginWindow,
		FExecuteAction::CreateRaw(this, &FCameraRecorderModule::PluginButtonClicked),
		FCanExecuteAction());

	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FCameraRecorderModule::RegisterMenus));
	
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(CameraRecorderTabName, FOnSpawnTab::CreateRaw(this, &FCameraRecorderModule::OnSpawnPluginTab))
		.SetDisplayName(LOCTEXT("FCameraRecorderTabTitle", "CameraRecorder"))
		.SetMenuType(ETabSpawnerMenuType::Hidden);

	// Register tick delegate
	TickDelegateHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FCameraRecorderModule::HandleTicker));

	UE_LOG(LogTemp, Warning, TEXT("===== CameraRecorder Module Started - Ticker Registered ====="));
}

void FCameraRecorderModule::ShutdownModule()
{
	// Stop any active recording
	if (bIsRecording)
	{
		bIsRecording = false;
		bIsInWarmup = false;
	}
	
	// Unregister tick delegate
	if (TickDelegateHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickDelegateHandle);
		TickDelegateHandle.Reset();
	}

	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);

	FCameraRecorderStyle::Shutdown();
	FCameraRecorderCommands::Unregister();

	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(CameraRecorderTabName);
	
	// Clear all weak pointers
	CameraRecorderWidget.Reset();
	CurrentLevelSequence.Reset();
	RecordingCamera.Reset();
	ActiveSequencer.Reset();
}

TSharedRef<SDockTab> FCameraRecorderModule::OnSpawnPluginTab(const FSpawnTabArgs& SpawnTabArgs)
{
	TSharedRef<SCameraRecorderWidget> Widget = SNew(SCameraRecorderWidget)
		.Module(this);
	
	CameraRecorderWidget = Widget;

	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			Widget
		];
}

void FCameraRecorderModule::PluginButtonClicked()
{
	FGlobalTabmanager::Get()->TryInvokeTab(CameraRecorderTabName);
}

TSharedPtr<ISequencer> FCameraRecorderModule::GetActiveSequencer()
{
	if (!GEditor)
	{
		return nullptr;
	}

	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!AssetEditorSubsystem)
	{
		return nullptr;
	}

	TArray<UObject*> EditedAssets = AssetEditorSubsystem->GetAllEditedAssets();
	
	for (UObject* Asset : EditedAssets)
	{
		if (ULevelSequence* LevelSeq = Cast<ULevelSequence>(Asset))
		{
			IAssetEditorInstance* AssetEditor = AssetEditorSubsystem->FindEditorForAsset(LevelSeq, false);
			if (ILevelSequenceEditorToolkit* LevelSequenceEditor = static_cast<ILevelSequenceEditorToolkit*>(AssetEditor))
			{
				return LevelSequenceEditor->GetSequencer();
			}
		}
	}
	
	return nullptr;
}

void FCameraRecorderModule::StartSequencerPlayback()
{
	TSharedPtr<ISequencer> Sequencer = GetActiveSequencer();
	if (!Sequencer.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to get Sequencer for playback!"));
		return;
	}

	ActiveSequencer = Sequencer;

	// Calculate the warmup start frame - ALLOW negative frames!
	WarmupStartFrame = StartFrame - WarmupFrames;

	// Get the movie scene to check/extend playback range
	if (CurrentLevelSequence.IsValid())
	{
		UMovieScene* MovieScene = CurrentLevelSequence->GetMovieScene();
		if (MovieScene)
		{
			// IMPORTANT: EndFrame represents theLAST FRAME to play, but Sequencer's range is exclusive
			// So we need to set the range end to EndFrame + 1
			int32 TotalEndFrame = EndFrame + 1;  // +1 because Sequencer range is exclusive
			
			// Get the display rate
			FFrameRate DisplayRate = MovieScene->GetDisplayRate();
			FFrameRate TickResolution = MovieScene->GetTickResolution();
			
			// Get theCURRENT playback range
			TRange<FFrameNumber> CurrentRange = MovieScene->GetPlaybackRange();
			
			// Convert our needed start/end to tick resolution
			FFrameNumber NeededStartInTicks = FFrameRate::TransformTime(
				FFrameTime(StartFrame),  // Start at StartFrame, NOT WarmupStartFrame
				DisplayRate, 
				TickResolution
			).FloorToFrame();
			
			FFrameNumber NeededEndInTicks = FFrameRate::TransformTime(
				FFrameTime(TotalEndFrame),  // Now using EndFrame + 1
				DisplayRate, 
				TickResolution
			).CeilToFrame();
			
			// Only extend the range if necessary, don't shrink it
			FFrameNumber FinalStartFrame = CurrentRange.GetLowerBoundValue();
			FFrameNumber FinalEndFrame = CurrentRange.GetUpperBoundValue();
			
			bool bRangeChanged = false;
			
			// Extend start if needed (make it earlier)
			if (NeededStartInTicks < FinalStartFrame)
			{
				FinalStartFrame = NeededStartInTicks;
				bRangeChanged = true;
			}
			
			// Extend end if needed (make it later)
			if (NeededEndInTicks > FinalEndFrame)
			{
				FinalEndFrame = NeededEndInTicks;
				bRangeChanged = true;
			}
			
			// Only update the range if we actually need to extend it
			if (bRangeChanged)
			{
				TRange<FFrameNumber> NewRange = TRange<FFrameNumber>(FinalStartFrame, FinalEndFrame);
				MovieScene->SetPlaybackRange(NewRange);
				
				UE_LOG(LogTemp, Warning, TEXT("Extended Sequencer playback range to [%d, %d] in tick resolution (display frames %d to %d)"),
					FinalStartFrame.Value, FinalEndFrame.Value, StartFrame, EndFrame);
			}
			else
			{
				UE_LOG(LogTemp, Log, TEXT("Current playback range [%d, %d] already contains recording range"),
					CurrentRange.GetLowerBoundValue().Value, CurrentRange.GetUpperBoundValue().Value);
			}
			
			// Set the view range to see the entire recording
			double ViewStart = FFrameRate::TransformTime(FFrameTime(WarmupStartFrame), DisplayRate, TickResolution).AsDecimal() / TickResolution.AsDecimal();
			double ViewEnd = FFrameRate::TransformTime(FFrameTime(EndFrame), DisplayRate, TickResolution).AsDecimal() / TickResolution.AsDecimal();
			
			Sequencer->SetViewRange(TRange<double>(ViewStart, ViewEnd), EViewRangeInterpolation::Immediate);
			
			// CRITICAL: Set global time in TICK RESOLUTION, not display frames
			// Convert WarmupStartFrame (display frame) to tick resolution
			FFrameNumber WarmupFrameInTicks = FFrameRate::TransformTime(
				FFrameTime(WarmupStartFrame),
				DisplayRate,
				TickResolution
			).FloorToFrame();
			
			UE_LOG(LogTemp, Warning, TEXT("Setting playhead to display frame %d (tick frame %d)"), 
				WarmupStartFrame, WarmupFrameInTicks.Value);
			
			// Set using FFrameTime with tick resolution
			Sequencer->SetGlobalTime(FFrameTime(WarmupFrameInTicks));
			
			// Force evaluation to lock in the time
			Sequencer->ForceEvaluate();
			
			// Now start playback
			Sequencer->SetPlaybackStatus(EMovieScenePlayerStatus::Playing);
			
			UE_LOG(LogTemp, Warning, TEXT("===== Started Sequencer playback from frame %d (warmup: %d frames) ====="), 
				WarmupStartFrame, WarmupFrames);
		}
	}
}

void FCameraRecorderModule::ClearExistingKeyframes(const FGuid& CameraBinding)
{
	if (!CurrentLevelSequence.IsValid() || !CameraBinding.IsValid())
	{
		return;
	}

	UMovieScene* MovieScene = CurrentLevelSequence->GetMovieScene();
	if (!MovieScene)
	{
		return;
	}

	// Find the transform track for this camera
	UMovieScene3DTransformTrack* TransformTrack = MovieScene->FindTrack<UMovieScene3DTransformTrack>(CameraBinding);
	if (!TransformTrack)
	{
		UE_LOG(LogTemp, Log, TEXT("No transform track found for camera, nothing to clear"));
		return;
	}

	// Get the transform section
	UMovieScene3DTransformSection* TransformSection = Cast<UMovieScene3DTransformSection>(
		TransformTrack->FindSection(0));
	
	if (!TransformSection)
	{
		UE_LOG(LogTemp, Log, TEXT("No transform section found, nothing to clear"));
		return;
	}

	// Convert frame range to tick resolution
	FFrameRate DisplayRate = MovieScene->GetDisplayRate();
	FFrameRate TickResolution = MovieScene->GetTickResolution();
	
	FFrameNumber StartFrameInTicks = FFrameRate::TransformTime(
		FFrameTime(StartFrame), 
		DisplayRate, 
		TickResolution
	).FloorToFrame();
	
	FFrameNumber EndFrameInTicks = FFrameRate::TransformTime(
		FFrameTime(EndFrame), 
		DisplayRate, 
		TickResolution
	).CeilToFrame();

	// Get all the channels
	TArrayView<FMovieSceneDoubleChannel*> Channels = TransformSection->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
	
	if (Channels.Num() < 6)
	{
		UE_LOG(LogTemp, Warning, TEXT("Transform section doesn't have enough channels"));
		return;
	}

	int32 TotalKeysRemoved = 0;

	// Clear keyframes in the recording range for all channels
	for (FMovieSceneDoubleChannel* Channel : Channels)
	{
		if (Channel)
		{
			TArray<FKeyHandle> KeyHandlesToRemove;
			
			// Get all keys in the channel
			TArrayView<const FFrameNumber> KeyTimes = Channel->GetTimes();
			TArrayView<const FMovieSceneDoubleValue> KeyValues = Channel->GetValues();
			
			// Find all key handles within the recording range
			for (int32 KeyIndex = 0; KeyIndex < KeyTimes.Num(); ++KeyIndex)
			{
				const FFrameNumber& KeyTime = KeyTimes[KeyIndex];
				if (KeyTime >= StartFrameInTicks && KeyTime <= EndFrameInTicks)
				{
					// Get the key handle at this index
					FKeyHandle KeyHandle = Channel->GetHandle(KeyIndex);
					if (KeyHandle != FKeyHandle::Invalid())
					{
						KeyHandlesToRemove.Add(KeyHandle);
					}
				}
			}
			
			// Remove the keys using their handles
			if (KeyHandlesToRemove.Num() > 0)
			{
				Channel->DeleteKeys(KeyHandlesToRemove);
				TotalKeysRemoved += KeyHandlesToRemove.Num();
			}
		}
	}

	if (TotalKeysRemoved > 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("Cleared %d existing keyframes in range [%d - %d]"), 
			TotalKeysRemoved, StartFrame, EndFrame);
		
		// Notify sequencer that data has changed
		if (TSharedPtr<ISequencer> Sequencer = ActiveSequencer.Pin())
		{
			Sequencer->NotifyMovieSceneDataChanged(EMovieSceneDataChangeType::TrackValueChanged);
		}
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("No existing keyframes found in range [%d - %d]"), 
			StartFrame, EndFrame);
	}
}

void FCameraRecorderModule::SetRecording(bool bInIsRecording)
{
	bIsRecording = bInIsRecording;
	
	if (bInIsRecording)
	{
		// Reset tracking variables
		bIsInWarmup = WarmupFrames > 0;
		LastRecordedFrame = -1;
		RecordedFrames.Empty(); // CHANGED: Use new array
		
		WarmupStartFrame = StartFrame - WarmupFrames;
		
		ULevelSequence* NewSequence = GetOrCreateLevelSequence();
		
		// CRITICAL: If the sequence changed, invalidate the cached binding
		if (CurrentLevelSequence.Get() != NewSequence)
		{
			UE_LOG(LogTemp, Warning, TEXT("Sequence changed - invalidating cached camera binding"));
			CachedCameraBinding.Invalidate();
		}
		
		CurrentLevelSequence = NewSequence;
		
		if (!CurrentLevelSequence.IsValid())
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to get or create Level Sequence!"));
			bIsRecording = false;
			bIsInWarmup = false;
			return;
		}
		
		if (!GEditor)
		{
			UE_LOG(LogTemp, Error, TEXT("GEditor is null!"));
			bIsRecording = false;
			bIsInWarmup = false;
			return;
		}

		ULevelEditorSubsystem* LevelEditorSubsystem = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
		if (!LevelEditorSubsystem)
		{
			UE_LOG(LogTemp, Error, TEXT("LevelEditorSubsystem is null!"));
			bIsRecording = false;
			bIsInWarmup = false;
			return;
		}

		AActor* PilotActor = LevelEditorSubsystem->GetPilotLevelActor();
		if (!PilotActor)
		{
			UE_LOG(LogTemp, Error, TEXT("No piloted actor found! Please pilot a camera before recording."));
			bIsRecording = false;
			bIsInWarmup = false;
			return;
		}

		ACineCameraActor* CineCam = Cast<ACineCameraActor>(PilotActor);
		if (!CineCam)
		{
			UE_LOG(LogTemp, Error, TEXT("Piloted actor is not a CineCameraActor!"));
			bIsRecording = false;
			bIsInWarmup = false;
			return;
		}

		// STEP 1: Capture the camera's current transform BEFORE doing anything
		FTransform CapturedTransform = CineCam->GetActorTransform();
		UE_LOG(LogTemp, Warning, TEXT("Captured camera transform: Loc=%s, Rot=%s"),
			*CapturedTransform.GetLocation().ToString(),
			*CapturedTransform.Rotator().ToString());

		// Get or create the camera binding
		FGuid CameraBinding = GetOrCreateCameraBinding(CineCam);
		if (!CameraBinding.IsValid())
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to get camera binding!"));
			bIsRecording = false;
			bIsInWarmup = false;
			return;
		}

		// Clear existing keyframes in the recording range
		ClearExistingKeyframes(CameraBinding);
		
		// STEP 2: PROPERLY disable the camera's transform track (like the mute button)
		UMovieScene* MovieScene = CurrentLevelSequence->GetMovieScene();
		if (MovieScene)
		{
			UMovieScene3DTransformTrack* TransformTrack = MovieScene->FindTrack<UMovieScene3DTransformTrack>(CameraBinding);
			if (TransformTrack)
			{
				// Disable the track itself
				TransformTrack->SetEvalDisabled(true);
				
				// CRITICAL: Also disable all sections in the track (this is what the mute button does)
				const TArray<UMovieSceneSection*>& Sections = TransformTrack->GetAllSections();
				for (UMovieSceneSection* Section : Sections)
				{
					if (Section)
					{
						Section->SetIsActive(false);
						UE_LOG(LogTemp, Log, TEXT("Disabled section in camera transform track"));
					}
				}
				
				UE_LOG(LogTemp, Warning, TEXT("Fully disabled camera transform track (track + sections)"));
			}
			else
			{
				UE_LOG(LogTemp, Log, TEXT("No transform track exists yet - will be created on first keyframe"));
			}
		}
		
		// Start sequencer playback
		StartSequencerPlayback();
		
		// STEP 3: Restore camera to captured transform (after track is disabled)
		CineCam->SetActorTransform(CapturedTransform);
		UE_LOG(LogTemp, Warning, TEXT("Restored camera to captured transform"));
		
		if (WarmupFrames > 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("===== Recording started with %d warmup frames (playing from frame %d) ====="), 
				WarmupFrames, WarmupStartFrame);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("===== Recording started with no warmup ====="));
		}
		UE_LOG(LogTemp, Warning, TEXT("===== Will record from frame %d to %d (step: %d) ====="), StartFrame, EndFrame, FrameStep);
		UE_LOG(LogTemp, Warning, TEXT("===== Recording to sequence: %s ====="), *CurrentLevelSequence->GetName());
	}
	else
	{
		StopRecording();
	}
}

void FCameraRecorderModule::StopRecording()
{
	bIsRecording = false;
	bIsInWarmup = false;
	
	// Stop sequencer playback
	if (TSharedPtr<ISequencer> Sequencer = ActiveSequencer.Pin())
	{
		Sequencer->SetPlaybackStatus(EMovieScenePlayerStatus::Stopped);
		UE_LOG(LogTemp, Warning, TEXT("===== Stopped Sequencer playback ====="));
	}
	
	// CRITICAL: Re-enable the camera's transform track AND sections before writing keyframes
	if (CurrentLevelSequence.IsValid() && CachedCameraBinding.IsValid())
	{
		UMovieScene* MovieScene = CurrentLevelSequence->GetMovieScene();
		if (MovieScene)
		{
			UMovieScene3DTransformTrack* TransformTrack = MovieScene->FindTrack<UMovieScene3DTransformTrack>(CachedCameraBinding);
			if (TransformTrack)
			{
				// Re-enable the track
				TransformTrack->SetEvalDisabled(false);
				
				// CRITICAL: Re-enable all sections (reverse what we did in SetRecording)
				const TArray<UMovieSceneSection*>& Sections = TransformTrack->GetAllSections();
				for (UMovieSceneSection* Section : Sections)
				{
					if (Section)
					{
						Section->SetIsActive(true);
						UE_LOG(LogTemp, Log, TEXT("Re-enabled section in camera transform track"));
					}
				}
				
				UE_LOG(LogTemp, Warning, TEXT("Re-enabled camera transform track (track + sections)"));
			}
		}
		
		// NOW write all the stored frames as keyframes
		if (RecordedFrames.Num() > 0)
		{
			// Apply rotation snap correction BEFORE writing keyframes
			ApplyRotationSnapCorrection();
			
			UE_LOG(LogTemp, Warning, TEXT("Writing %d stored keyframes to sequence..."), RecordedFrames.Num());
			
			for (const FRecordedCameraFrame& Frame : RecordedFrames)
			{
				RecordCameraKeyframeWithRotation(Frame.Location, Frame.Rotation, Frame.FrameNumber);
			}
			
			UE_LOG(LogTemp, Warning, TEXT("Successfully wrote %d keyframes!"), RecordedFrames.Num());
			
			// Clear the stored data
			RecordedFrames.Empty();
		}
	}
	
	// Notify widget to update button state
	if (TSharedPtr<SCameraRecorderWidget> Widget = CameraRecorderWidget.Pin())
	{
		Widget->OnRecordingStopped();
	}
	
	UE_LOG(LogTemp, Warning, TEXT("===== Recording stopped at frame %d ====="), CurrentFrame);
	
	// DON'T reset CurrentLevelSequence - we need it for comparison next time!
	RecordingCamera.Reset();
	ActiveSequencer.Reset();
	LastRecordedFrame = -1;
	
	// CachedCameraBinding stays valid for next recording
}

ULevelSequence* FCameraRecorderModule::GetOrCreateLevelSequence()
{
	TSharedPtr<ISequencer> Sequencer = GetActiveSequencer();
	if (!Sequencer.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("No active Sequencer found!"));
		return nullptr;
	}

	// Get the currently focused/edited Level Sequence
	UMovieSceneSequence* FocusedSequence = Sequencer->GetFocusedMovieSceneSequence();
	if (!FocusedSequence)
	{
		UE_LOG(LogTemp, Error, TEXT("No Level Sequence is currently open in Sequencer!"));
		return nullptr;
	}

	// Cast to ULevelSequence (most common type)
	ULevelSequence* LevelSeq = Cast<ULevelSequence>(FocusedSequence);
	if (!LevelSeq)
	{
		UE_LOG(LogTemp, Error, TEXT("Focused sequence is not a Level Sequence!"));
		return nullptr;
	}

	UE_LOG(LogTemp, Log, TEXT("Using Level Sequence: %s"), *LevelSeq->GetName());
	return LevelSeq;
}

FGuid FCameraRecorderModule::GetOrCreateCameraBinding(ACineCameraActor* CineCam)
{
	if (!CurrentLevelSequence.IsValid() || !CineCam)
	{
		return FGuid();
	}

	// Return cached binding if valid and verify it still points to this camera
	if (CachedCameraBinding.IsValid())
	{
		// Validate the cached binding still points to this camera
		UMovieScene* MovieScene = CurrentLevelSequence->GetMovieScene();
		if (MovieScene)
		{
			TArray<UObject*, TInlineAllocator<1>> BoundObjects;
			CurrentLevelSequence->LocateBoundObjects(CachedCameraBinding, CineCam->GetWorld(), BoundObjects);
			
			for (UObject* BoundObject : BoundObjects)
			{
				if (BoundObject == CineCam)
				{
					UE_LOG(LogTemp, Log, TEXT("Reusing cached camera binding for %s"), *CineCam->GetActorLabel());
					return CachedCameraBinding;
				}
			}
			
			UE_LOG(LogTemp, Warning, TEXT("Cached binding is stale, searching for existing binding..."));
			CachedCameraBinding.Invalidate();
		}
	}

	UMovieScene* MovieScene = CurrentLevelSequence->GetMovieScene();
	if (!MovieScene)
	{
		return FGuid();
	}

	// First, search for existing possessables by comparing bound objects
	for (int32 i = 0; i < MovieScene->GetPossessableCount(); ++i)
	{
		const FMovieScenePossessable& Possessable = MovieScene->GetPossessable(i);
		TArray<UObject*, TInlineAllocator<1>> BoundObjects;
		
		CurrentLevelSequence->LocateBoundObjects(Possessable.GetGuid(), CineCam->GetWorld(), BoundObjects);
		
		for (UObject* BoundObject : BoundObjects)
		{
			if (BoundObject == CineCam)
			{
				CachedCameraBinding = Possessable.GetGuid();
				UE_LOG(LogTemp, Log, TEXT("Found existing camera binding for %s (GUID: %s)"), 
					*CineCam->GetActorLabel(), *CachedCameraBinding.ToString());
				return CachedCameraBinding;
			}
		}
	}
	
	// Second, check if the camera is already bound as a spawnable (less common but possible)
	for (int32 i = 0; i < MovieScene->GetSpawnableCount(); ++i)
	{
		const FMovieSceneSpawnable& Spawnable = MovieScene->GetSpawnable(i);
		TArray<UObject*, TInlineAllocator<1>> BoundObjects;
		
		CurrentLevelSequence->LocateBoundObjects(Spawnable.GetGuid(), CineCam->GetWorld(), BoundObjects);
		
		for (UObject* BoundObject : BoundObjects)
		{
			if (BoundObject == CineCam)
			{
				CachedCameraBinding = Spawnable.GetGuid();
				UE_LOG(LogTemp, Log, TEXT("Found existing camera spawnable for %s (GUID: %s)"), 
					*CineCam->GetActorLabel(), *CachedCameraBinding.ToString());
				return CachedCameraBinding;
			}
		}
	}
	
	// Third, try to find by name match (fallback for edge cases)
	FString CameraName = CineCam->GetActorLabel();
	for (int32 i = 0; i < MovieScene->GetPossessableCount(); ++i)
	{
		const FMovieScenePossessable& Possessable = MovieScene->GetPossessable(i);
		
		// Check if the name matches
		if (Possessable.GetName() == CameraName)
		{
			// Verify this is actually a camera actor class
			if (Possessable.GetPossessedObjectClass()->IsChildOf(ACineCameraActor::StaticClass()))
			{
				// CRITICAL FIX: Don't call BindPossessableObject - it ADDS to the array!
				// Just cache and return the GUID we found
				CachedCameraBinding = Possessable.GetGuid();
				UE_LOG(LogTemp, Log, TEXT("Found existing camera binding by name match: %s (GUID: %s)"), 
					*CameraName, *CachedCameraBinding.ToString());
				return CachedCameraBinding;
			}
		}
	}
	
	// Camera not in sequence yet, add it as a new possessable
	CachedCameraBinding = MovieScene->AddPossessable(CineCam->GetActorLabel(), CineCam->GetClass());
	CurrentLevelSequence->BindPossessableObject(CachedCameraBinding, *CineCam, CineCam->GetWorld());
	UE_LOG(LogTemp, Warning, TEXT("Created NEW camera binding for %s (GUID: %s)"), 
		*CineCam->GetActorLabel(), *CachedCameraBinding.ToString());
	
	return CachedCameraBinding;
}

void FCameraRecorderModule::RecordCameraKeyframe(ACineCameraActor* CineCam, int32 FrameNumber)
{
	if (!CurrentLevelSequence.IsValid() || !CineCam)
	{
		return;
	}

	UMovieScene* MovieScene = CurrentLevelSequence->GetMovieScene();
	if (!MovieScene)
	{
		return;
	}

	// Get or create the camera binding (cached after first call)
	FGuid CameraBinding = GetOrCreateCameraBinding(CineCam);
	if (!CameraBinding.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to get camera binding!"));
		return;
	}

	// Get or create transform track
	UMovieScene3DTransformTrack* TransformTrack = MovieScene->FindTrack<UMovieScene3DTransformTrack>(CameraBinding);
	
	if (!TransformTrack)
	{
		TransformTrack = MovieScene->AddTrack<UMovieScene3DTransformTrack>(CameraBinding);
	}

	if (!TransformTrack)
	{
		return;
	}

	// Get or create section
	bool bSectionAdded = false;
	UMovieScene3DTransformSection* TransformSection = Cast<UMovieScene3DTransformSection>(
		TransformTrack->FindOrAddSection(0, bSectionAdded));
	
	if (!TransformSection)
	{
		return;
	}

	if (bSectionAdded)
	{
		TransformSection->SetRange(TRange<FFrameNumber>::All());
	}

	// Get transform
	FTransform Transform = CineCam->GetActorTransform();
	FVector Location = Transform.GetLocation();
	FRotator Rotation = Transform.Rotator();

	// Convert frame to frame number
	FFrameRate FrameRate = MovieScene->GetTickResolution();
	FFrameNumber FrameNum = FFrameRate::TransformTime(FFrameTime(FrameNumber), MovieScene->GetDisplayRate(), FrameRate).FloorToFrame();

	// Get all channels
	TArrayView<FMovieSceneDoubleChannel*> Channels = TransformSection->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
	
	if (Channels.Num() >= 6)
	{
		// Determine how to add keys based on interpolation mode
		switch (InterpMode)
		{
			case ECameraRecorderInterpMode::Linear:
				// Use AddLinearKey for linear interpolation
				Channels[0]->AddLinearKey(FrameNum, Location.X);
				Channels[1]->AddLinearKey(FrameNum, Location.Y);
				Channels[2]->AddLinearKey(FrameNum, Location.Z);
				Channels[3]->AddLinearKey(FrameNum, Rotation.Roll);
				Channels[4]->AddLinearKey(FrameNum, Rotation.Pitch);
				Channels[5]->AddLinearKey(FrameNum, Rotation.Yaw);
				break;
				
			case ECameraRecorderInterpMode::Constant:
				// Use AddConstantKey for constant interpolation
				Channels[0]->AddConstantKey(FrameNum, Location.X);
				Channels[1]->AddConstantKey(FrameNum, Location.Y);
				Channels[2]->AddConstantKey(FrameNum, Location.Z);
				Channels[3]->AddConstantKey(FrameNum, Rotation.Roll);
				Channels[4]->AddConstantKey(FrameNum, Rotation.Pitch);
				Channels[5]->AddConstantKey(FrameNum, Rotation.Yaw);
				break;
				
			case ECameraRecorderInterpMode::Auto:
			case ECameraRecorderInterpMode::User:
			case ECameraRecorderInterpMode::Break:
			default:
				// Use AddCubicKey for cubic interpolation (Auto, User, Break)
				// AddCubicKey returns the index, we need to get the handle and modify tangent mode
				{
					// Add the keys and get their indices
					int32 KeyIndices[6];
					KeyIndices[0] = Channels[0]->AddCubicKey(FrameNum, Location.X);
					KeyIndices[1] = Channels[1]->AddCubicKey(FrameNum, Location.Y);
					KeyIndices[2] = Channels[2]->AddCubicKey(FrameNum, Location.Z);
					KeyIndices[3] = Channels[3]->AddCubicKey(FrameNum, Rotation.Roll);
					KeyIndices[4] = Channels[4]->AddCubicKey(FrameNum, Rotation.Pitch);
					KeyIndices[5] = Channels[5]->AddCubicKey(FrameNum, Rotation.Yaw);
					
					// Set the tangent mode for cubic keys
					ERichCurveTangentMode TangentMode = RCTM_Auto;
					
					if (InterpMode == ECameraRecorderInterpMode::User)
					{
						TangentMode = RCTM_User;
					}
					else if (InterpMode == ECameraRecorderInterpMode::Break)
					{
						TangentMode = RCTM_Break;
					}
					
					// Apply tangent mode to all channels
					for (int32 i = 0; i < 6; ++i)
					{
						if (KeyIndices[i] != INDEX_NONE)
						{
							TMovieSceneChannelData<FMovieSceneDoubleValue> ChannelData = Channels[i]->GetData();
							TArrayView<FMovieSceneDoubleValue> Values = ChannelData.GetValues();
							
							if (KeyIndices[i] < Values.Num())
							{
								Values[KeyIndices[i]].TangentMode = TangentMode;
							}
						}
					}
				}
				break;
		}
	}
}

void FCameraRecorderModule::RecordCameraKeyframeWithRotation(const FVector& Location, const FRotator& Rotation, int32 FrameNumber)
{
	if (!CurrentLevelSequence.IsValid())
	{
		return;
	}

	UMovieScene* MovieScene = CurrentLevelSequence->GetMovieScene();
	if (!MovieScene)
	{
		return;
	}

	// Get the cached camera binding
	FGuid CameraBinding = CachedCameraBinding;
	if (!CameraBinding.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to get camera binding!"));
		return;
	}

	// Get or create transform track
	UMovieScene3DTransformTrack* TransformTrack = MovieScene->FindTrack<UMovieScene3DTransformTrack>(CameraBinding);
	
	if (!TransformTrack)
	{
		TransformTrack = MovieScene->AddTrack<UMovieScene3DTransformTrack>(CameraBinding);
	}

	if (!TransformTrack)
	{
		return;
	}

	// Get or create section
	bool bSectionAdded = false;
	UMovieScene3DTransformSection* TransformSection = Cast<UMovieScene3DTransformSection>(
		TransformTrack->FindOrAddSection(0, bSectionAdded));
	
	if (!TransformSection)
	{
		return;
	}

	if (bSectionAdded)
	{
		TransformSection->SetRange(TRange<FFrameNumber>::All());
	}

	// Convert frame to frame number
	FFrameRate FrameRate = MovieScene->GetTickResolution();
	FFrameNumber FrameNum = FFrameRate::TransformTime(FFrameTime(FrameNumber), MovieScene->GetDisplayRate(), FrameRate).FloorToFrame();

	// Get all channels
	TArrayView<FMovieSceneDoubleChannel*> Channels = TransformSection->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
	
	if (Channels.Num() >= 6)
	{
		// DIRECTLY write the unwrapped euler angles to the channels
		switch (InterpMode)
		{
			case ECameraRecorderInterpMode::Linear:
				Channels[0]->AddLinearKey(FrameNum, Location.X);
				Channels[1]->AddLinearKey(FrameNum, Location.Y);
				Channels[2]->AddLinearKey(FrameNum, Location.Z);
				Channels[3]->AddLinearKey(FrameNum, Rotation.Roll);
				Channels[4]->AddLinearKey(FrameNum, Rotation.Pitch);
				Channels[5]->AddLinearKey(FrameNum, Rotation.Yaw);
				break;
				
			case ECameraRecorderInterpMode::Constant:
				Channels[0]->AddConstantKey(FrameNum, Location.X);
				Channels[1]->AddConstantKey(FrameNum, Location.Y);
				Channels[2]->AddConstantKey(FrameNum, Location.Z);
				Channels[3]->AddConstantKey(FrameNum, Rotation.Roll);
				Channels[4]->AddConstantKey(FrameNum, Rotation.Pitch);
				Channels[5]->AddConstantKey(FrameNum, Rotation.Yaw);
				break;
				
			case ECameraRecorderInterpMode::Auto:
			case ECameraRecorderInterpMode::User:
			case ECameraRecorderInterpMode::Break:
			default:
				{
					int32 KeyIndices[6];
					KeyIndices[0] = Channels[0]->AddCubicKey(FrameNum, Location.X);
					KeyIndices[1] = Channels[1]->AddCubicKey(FrameNum, Location.Y);
					KeyIndices[2] = Channels[2]->AddCubicKey(FrameNum, Location.Z);
					KeyIndices[3] = Channels[3]->AddCubicKey(FrameNum, Rotation.Roll);
					KeyIndices[4] = Channels[4]->AddCubicKey(FrameNum, Rotation.Pitch);
					KeyIndices[5] = Channels[5]->AddCubicKey(FrameNum, Rotation.Yaw);
					
					ERichCurveTangentMode TangentMode = RCTM_Auto;
					
					if (InterpMode == ECameraRecorderInterpMode::User)
					{
						TangentMode = RCTM_User;
					}
					else if (InterpMode == ECameraRecorderInterpMode::Break)
					{
						TangentMode = RCTM_Break;
					}
					
					for (int32 i = 0; i < 6; ++i)
					{
						if (KeyIndices[i] != INDEX_NONE)
						{
							TMovieSceneChannelData<FMovieSceneDoubleValue> ChannelData = Channels[i]->GetData();
							TArrayView<FMovieSceneDoubleValue> Values = ChannelData.GetValues();
							
							if (KeyIndices[i] < Values.Num())
							{
								Values[KeyIndices[i]].TangentMode = TangentMode;
							}
						}
					}
				}
				break;
		}
	}
}

void FCameraRecorderModule::ApplyRotationSnapCorrection()
{
	if (!bSnapRotationCorrection || RecordedFrames.Num() < 2)
	{
		UE_LOG(LogTemp, Warning, TEXT("Snap correction skipped - Enabled: %s, Frames: %d"), 
			bSnapRotationCorrection ? TEXT("YES") : TEXT("NO"), 
			RecordedFrames.Num());
		return;
	}

	UE_LOG(LogTemp, Warning, TEXT("===== STARTING ROTATION SNAP CORRECTION ON %d FRAMES ====="), RecordedFrames.Num());

	double PitchOffset = 0.0;
	double YawOffset = 0.0;
	double RollOffset = 0.0;
	int32 SnapsDetected = 0;
	int32 FramesCorrected = 0;

	FRotator PrevRotation = RecordedFrames[0].Rotation;
	
	UE_LOG(LogTemp, Warning, TEXT("Frame %d: Reference rotation (P:%.2f Y:%.2f R:%.2f) - NO CHANGES"),
		RecordedFrames[0].FrameNumber, PrevRotation.Pitch, PrevRotation.Yaw, PrevRotation.Roll);

	for (int32 i = 1; i < RecordedFrames.Num(); ++i)
	{
		FRotator& CurrentRotation = RecordedFrames[i].Rotation;
		
		// Store original values for logging
		FRotator OriginalRotation = CurrentRotation;
		
		double DeltaPitch = CurrentRotation.Pitch - PrevRotation.Pitch;
		double DeltaYaw = CurrentRotation.Yaw - PrevRotation.Yaw;
		double DeltaRoll = CurrentRotation.Roll - PrevRotation.Roll;

		bool bSnapDetected = false;

		// Detect snaps
		if (DeltaPitch > 180.0)
		{
			PitchOffset -= 360.0;
			bSnapDetected = true;
			UE_LOG(LogTemp, Warning, TEXT("  >>> PITCH SNAP at frame %d! Delta %.2f > 180° -> offset -360°"), 
				RecordedFrames[i].FrameNumber, DeltaPitch);
		}
		else if (DeltaPitch < -180.0)
		{
			PitchOffset += 360.0;
			bSnapDetected = true;
			UE_LOG(LogTemp, Warning, TEXT("  >>> PITCH SNAP at frame %d! Delta %.2f < -180° -> offset +360°"), 
				RecordedFrames[i].FrameNumber, DeltaPitch);
		}
		
		if (DeltaYaw > 180.0)
		{
			YawOffset -= 360.0;
			bSnapDetected = true;
			UE_LOG(LogTemp, Warning, TEXT("  >>> YAW SNAP at frame %d! Delta %.2f > 180° -> offset -360°"), 
				RecordedFrames[i].FrameNumber, DeltaYaw);
		}
		else if (DeltaYaw < -180.0)
		{
			YawOffset += 360.0;
			bSnapDetected = true;
			UE_LOG(LogTemp, Warning, TEXT("  >>> YAW SNAP at frame %d! Delta %.2f < -180° -> offset +360°"), 
				RecordedFrames[i].FrameNumber, DeltaYaw);
		}
		
		if (DeltaRoll > 180.0)
		{
			RollOffset -= 360.0;
			bSnapDetected = true;
			UE_LOG(LogTemp, Warning, TEXT("  >>> ROLL SNAP at frame %d! Delta %.2f > 180° -> offset -360°"), 
				RecordedFrames[i].FrameNumber, DeltaRoll);
		}
		else if (DeltaRoll < -180.0)
		{
			RollOffset += 360.0;
			bSnapDetected = true;
			UE_LOG(LogTemp, Warning, TEXT("  >>> ROLL SNAP at frame %d! Delta %.2f < -180° -> offset +360°"), 
				RecordedFrames[i].FrameNumber, DeltaRoll);
		}

		if (bSnapDetected)
		{
			SnapsDetected++;
		}

		// Apply accumulated offsets DIRECTLY to the rotation
		CurrentRotation.Pitch += PitchOffset;
		CurrentRotation.Yaw += YawOffset;
		CurrentRotation.Roll += RollOffset;

		// Check if ANY offset is active (frame was modified)
		bool bFrameModified = (PitchOffset != 0.0 || YawOffset != 0.0 || RollOffset != 0.0);

		if (bFrameModified)
		{
			FramesCorrected++;
			
			UE_LOG(LogTemp, Warning, TEXT("  Frame %d MODIFIED:"), RecordedFrames[i].FrameNumber);
			UE_LOG(LogTemp, Warning, TEXT("    BEFORE: Pitch=%.2f°  Yaw=%.2f°  Roll=%.2f°"), 
				OriginalRotation.Pitch, OriginalRotation.Yaw, OriginalRotation.Roll);
			UE_LOG(LogTemp, Warning, TEXT("    AFTER:  Pitch=%.2f°  Yaw=%.2f°  Roll=%.2f°"), 
				CurrentRotation.Pitch, CurrentRotation.Yaw, CurrentRotation.Roll);
			UE_LOG(LogTemp, Warning, TEXT("    CHANGE: Pitch%+.0f°  Yaw%+.0f°  Roll%+.0f°"), 
				PitchOffset, YawOffset, RollOffset);
		}
		else
		{
			UE_LOG(LogTemp, Log, TEXT("  Frame %d: No changes (no accumulated offset)"), 
				RecordedFrames[i].FrameNumber);
		}

		// CRITICAL: Store the RAW rotation for next iteration's comparison (not corrected!)
		PrevRotation = OriginalRotation;
	}

	UE_LOG(LogTemp, Warning, TEXT("===== ROTATION SNAP CORRECTION COMPLETE ====="));
	UE_LOG(LogTemp, Warning, TEXT("  Snaps detected: %d"), SnapsDetected);
	UE_LOG(LogTemp, Warning, TEXT("  Frames modified: %d out of %d"), FramesCorrected, RecordedFrames.Num());
	UE_LOG(LogTemp, Warning, TEXT("  Final accumulated offsets: Pitch=%+.0f° Yaw=%+.0f° Roll=%+.0f°"), 
		PitchOffset, YawOffset, RollOffset);
}

bool FCameraRecorderModule::HandleTicker(float DeltaTime)
{
	if (bIsRecording)
	{
		OnTick();
	}
	return true;
}

void FCameraRecorderModule::OnTick()
{
	if (!bIsRecording)
	{
		return;
	}

	TSharedPtr<ISequencer> Sequencer = ActiveSequencer.Pin();
	if (!Sequencer.IsValid())
	{
		return;
	}

	// Get current sequencer time
	FQualifiedFrameTime CurrentTime = Sequencer->GetGlobalTime();
	UMovieScene* MovieScene = CurrentLevelSequence.IsValid() ? CurrentLevelSequence->GetMovieScene() : nullptr;
	
	if (!MovieScene)
	{
		return;
	}

	// Convert from tick resolution to display frames
	FFrameRate DisplayRate = MovieScene->GetDisplayRate();
	FFrameRate TickResolution = MovieScene->GetTickResolution();
	
	FFrameTime DisplayFrameTime = FFrameRate::TransformTime(CurrentTime.Time, TickResolution, DisplayRate);
	CurrentFrame = DisplayFrameTime.FloorToFrame().Value;

	// Check if we're still in warmup
	if (bIsInWarmup)
	{
		if (CurrentFrame >= StartFrame)
		{
			bIsInWarmup = false;
			UE_LOG(LogTemp, Warning, TEXT("===== Warmup complete at frame %d, starting recording ====="), CurrentFrame);
		}
		return;  // Don't record during warmup
	}

	// Check if we've reached the end
	if (CurrentFrame > EndFrame)
	{
		// Add final keyframe if enabled
		if (bKeyframeOnLastFrame && LastRecordedFrame != EndFrame)
		{
			if (!GEditor)
			{
				return;
			}

			ULevelEditorSubsystem* LevelEditorSubsystem = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
			if (LevelEditorSubsystem)
			{
				AActor* PilotActor = LevelEditorSubsystem->GetPilotLevelActor();
				ACineCameraActor* CineCam = Cast<ACineCameraActor>(PilotActor);
				
				if (CineCam)
				{
					// Store final frame
					RecordedFrames.Add(FRecordedCameraFrame(EndFrame, CineCam->GetActorTransform()));
					UE_LOG(LogTemp, Warning, TEXT("Added final keyframe at frame %d"), EndFrame);
				}
			}
		}
		
		UE_LOG(LogTemp, Warning, TEXT("===== Reached end frame %d, stopping recording ====="), EndFrame);
		SetRecording(false);
		return;
	}

	// Check if we should record this frame
	bool bShouldRecord = (CurrentFrame >= StartFrame) && 
	                     (CurrentFrame <= EndFrame) &&
	                     ((CurrentFrame - StartFrame) % FrameStep == 0) &&
	                     (CurrentFrame != LastRecordedFrame);

	if (bShouldRecord)
	{
		if (!GEditor)
		{
			return;
		}

		ULevelEditorSubsystem* LevelEditorSubsystem = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
		if (!LevelEditorSubsystem)
		{
			return;
		}

		AActor* PilotActor = LevelEditorSubsystem->GetPilotLevelActor();
		ACineCameraActor* CineCam = Cast<ACineCameraActor>(PilotActor);
		
		if (CineCam)
		{
			// Store the frame
			RecordedFrames.Add(FRecordedCameraFrame(CurrentFrame, CineCam->GetActorTransform()));
			
			int32 KeyframeIndex = RecordedFrames.Num() - 1;
			UE_LOG(LogTemp, Log, TEXT("[Keyframe %d | Frame %d] Stored transform (will write after recording)"), 
				KeyframeIndex, CurrentFrame);
			
			LastRecordedFrame = CurrentFrame;
		}
	}
}

void FCameraRecorderModule::RegisterMenus()
{
	// Empty implementation - menus can be added here later if needed
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FCameraRecorderModule, CameraRecorder)