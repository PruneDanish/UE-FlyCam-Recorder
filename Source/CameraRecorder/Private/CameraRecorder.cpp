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
			
			// Get the CURRENT playback range
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
		CachedCameraBinding.Invalidate(); // Reset the cached binding
		
		// Calculate and store the warmup start frame - ALLOW negative frames!
		WarmupStartFrame = StartFrame - WarmupFrames;
		
		CurrentLevelSequence = GetOrCreateLevelSequence();
		
		if (!CurrentLevelSequence.IsValid())
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to get or create Level Sequence!"));
			bIsRecording = false;
			bIsInWarmup = false;
			return;
		}
		
		// Get or create camera binding first (needed for clearing keyframes)
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
		
		// IMPORTANT: DON'T record initial keyframe yet if we have warmup
		// Only record it if there's no warmup, otherwise record it when warmup completes
		if (WarmupFrames == 0)
		{
			RecordCameraKeyframe(CineCam, StartFrame);  // Records at frame 0
			StartSequencerPlayback();  // Starts at frame -30
			UE_LOG(LogTemp, Log, TEXT("Recorded initial camera position at frame %d (no warmup)"), StartFrame);
		}
		else
		{
			UE_LOG(LogTemp, Log, TEXT("Skipping initial keyframe - will record after %d warmup frames"), WarmupFrames);
		}
		
		// Start sequencer playback
		StartSequencerPlayback();
		
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
	
	// Notify widget to update button state
	if (TSharedPtr<SCameraRecorderWidget> Widget = CameraRecorderWidget.Pin())
	{
		Widget->OnRecordingStopped();
	}
	
	UE_LOG(LogTemp, Warning, TEXT("===== Recording stopped at frame %d ====="), CurrentFrame);
	
	CurrentLevelSequence.Reset();
	RecordingCamera.Reset();
	ActiveSequencer.Reset();
	LastRecordedFrame = -1;
	CachedCameraBinding.Invalidate(); // Clear the cached binding
}

FGuid FCameraRecorderModule::GetOrCreateCameraBinding(ACineCameraActor* CineCam)
{
	if (!CurrentLevelSequence.IsValid() || !CineCam)
	{
		return FGuid();
	}

	// Return cached binding if valid
	if (CachedCameraBinding.IsValid())
	{
		return CachedCameraBinding;
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
				// Try to rebind it to our camera
				CurrentLevelSequence->BindPossessableObject(Possessable.GetGuid(), *CineCam, CineCam->GetWorld());
				
				CachedCameraBinding = Possessable.GetGuid();
				UE_LOG(LogTemp, Log, TEXT("Found existing camera binding by name match and rebound: %s (GUID: %s)"), 
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

void FCameraRecorderModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	{
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Window");
		{
			FToolMenuSection& Section = Menu->FindOrAddSection("WindowLayout");
			Section.AddMenuEntryWithCommandList(FCameraRecorderCommands::Get().OpenPluginWindow, PluginCommands);
		}
	}

	{
		UToolMenu* ToolbarMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.PlayToolBar");
		{
			FToolMenuSection& Section = ToolbarMenu->FindOrAddSection("PluginTools");
			{
				FToolMenuEntry& Entry = Section.AddEntry(FToolMenuEntry::InitToolBarButton(FCameraRecorderCommands::Get().OpenPluginWindow));
				Entry.SetCommandList(PluginCommands);
			}
		}
	}
}

bool FCameraRecorderModule::HandleTicker(float DeltaTime)
{
	OnTick();
	return true; // Continue ticking
}

void FCameraRecorderModule::OnTick()
{
	if (!bIsRecording)
	{
		return;
	}

	// Get the current Sequencer frame
	TSharedPtr<ISequencer> Sequencer = ActiveSequencer.Pin();
	if (!Sequencer.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("Sequencer is no longer valid, stopping recording"));
		StopRecording();
		return;
	}

	// Check if sequencer stopped playing (but don't stop recording immediately - keep it playing)
	if (Sequencer->GetPlaybackStatus() != EMovieScenePlayerStatus::Playing)
	{
		// Try to restart playback instead of stopping
		UE_LOG(LogTemp, Warning, TEXT("Sequencer paused/stopped, attempting to restart playback"));
		Sequencer->SetPlaybackStatus(EMovieScenePlayerStatus::Playing);
		
		// If it still won't play, then stop recording
		if (Sequencer->GetPlaybackStatus() != EMovieScenePlayerStatus::Playing)
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to restart Sequencer playback, stopping recording"));
			StopRecording();
			return;
		}
	}

	// Get current frame from Sequencer - convert from tick resolution to display rate
	FFrameRate DisplayRate = Sequencer->GetFocusedDisplayRate();
	FFrameTime SequencerTime = Sequencer->GetGlobalTime().Time;
	
	// Get the movie scene to access tick resolution
	if (!CurrentLevelSequence.IsValid())
	{
		return;
	}
	
	UMovieScene* MovieScene = CurrentLevelSequence->GetMovieScene();
	if (!MovieScene)
	{
		return;
	}
	
	FFrameRate TickResolution = MovieScene->GetTickResolution();
	
	// Convert from tick resolution to display rate frames
	FFrameTime DisplayTime = FFrameRate::TransformTime(SequencerTime, TickResolution, DisplayRate);
	CurrentFrame = DisplayTime.FloorToFrame().Value;

	// DEBUG: Log every frame during warmup
	if (bIsInWarmup)
	{
		UE_LOG(LogTemp, Warning, TEXT("WARMUP TICK - CurrentFrame: %d, StartFrame: %d, WarmupStartFrame: %d, bIsInWarmup: %s"),
			CurrentFrame, StartFrame, WarmupStartFrame, bIsInWarmup ? TEXT("TRUE") : TEXT("FALSE"));
	}

	// Handle warmup period - check if we've reached the actual start frame
	if (bIsInWarmup)
	{
		if (CurrentFrame >= StartFrame)
		{
			bIsInWarmup = false;
			UE_LOG(LogTemp, Warning, TEXT("===== Warmup complete at frame %d, starting recording ====="), CurrentFrame);
		}
		else
		{
			// Still in warmup - log to show progress
			UE_LOG(LogTemp, Log, TEXT("Warmup in progress: frame %d (need to reach %d)"), 
				CurrentFrame, StartFrame);
			// Don't record yet
			return;
		}
	}

	// Check if we've reached or passed the end frame
	if (EndFrame > 0 && CurrentFrame >= EndFrame)
	{
		UE_LOG(LogTemp, Warning, TEXT("===== Reached end frame %d, stopping recording ====="), EndFrame);
		StopRecording();
		return;
	}

	// Check if this frame should be recorded based on frame step
	if ((CurrentFrame - StartFrame) % FrameStep != 0)
	{
		return;
	}

	// Prevent recording the same frame multiple times
	if (CurrentFrame == LastRecordedFrame)
	{
		return;
	}

	if (!GEditor)
	{
		return;
	}

	ULevelEditorSubsystem* LevelEditorSubsystem =
		GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();

	if (!LevelEditorSubsystem)
	{
		return;
	}

	AActor* PilotActor = LevelEditorSubsystem->GetPilotLevelActor();

	if (!PilotActor)
	{
		return;
	}

	ACineCameraActor* CineCam = Cast<ACineCameraActor>(PilotActor);

	if (!CineCam)
	{
		return;
	}

	// Store the recording camera reference
	if (!RecordingCamera.IsValid())
	{
		RecordingCamera = CineCam;
	}

	// Record keyframe to sequencer
	RecordCameraKeyframe(CineCam, CurrentFrame);
	LastRecordedFrame = CurrentFrame;

	const int32 KeyframeNumber = (CurrentFrame - StartFrame) / FrameStep;

	UE_LOG(LogTemp, Log, TEXT("[Keyframe %d | Frame %d] Recorded to Sequencer"),
		KeyframeNumber,
		CurrentFrame);
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FCameraRecorderModule, CameraRecorder)