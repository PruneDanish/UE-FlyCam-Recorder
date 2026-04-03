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
#include "Framework/Application/SlateApplication.h"

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
}

void FCameraRecorderModule::ShutdownModule()
{
	// Stop any active recording
	if (bIsRecording)
	{
		bIsRecording = false;
		bIsInCountdown = false;
		bWaitingForViewportClick = false;
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

void FCameraRecorderModule::StartCountdown()
{
	bWaitingForViewportClick = false;
	bIsInCountdown = true;
	CountdownTimer = 3.0f;
	CountdownSecondsRemaining = 3;
	
	UE_LOG(LogTemp, Warning, TEXT("===== Starting 3-second countdown ====="));
}

void FCameraRecorderModule::UpdateCountdown(float DeltaTime)
{
	if (!bIsInCountdown)
	{
		return;
	}
	
	CountdownTimer -= DeltaTime;
	int32 NewSecondsRemaining = FMath::CeilToInt(CountdownTimer);
	
	// Update the display if seconds changed
	if (NewSecondsRemaining != CountdownSecondsRemaining)
	{
		CountdownSecondsRemaining = FMath::Max(0, NewSecondsRemaining);
		UE_LOG(LogTemp, Warning, TEXT("Countdown: %d..."), CountdownSecondsRemaining);
	}
	
	// Countdown finished!
	if (CountdownTimer <= 0.0f)
	{
		bIsInCountdown = false;
		CountdownSecondsRemaining = 0;
		
		UE_LOG(LogTemp, Warning, TEXT("===== Countdown complete! Starting recording ====="));
		
		// NOW start the sequencer playback and actual recording
		StartSequencerPlayback();
	}
}

void FCameraRecorderModule::StartSequencerPlayback()
{
	TSharedPtr<ISequencer> Sequencer = ActiveSequencer.Pin();
	if (!Sequencer.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to get Sequencer for playback!"));
		return;
	}

	// Get the movie scene to extend playback range if needed
	if (CurrentLevelSequence.IsValid())
	{
		UMovieScene* MovieScene = CurrentLevelSequence->GetMovieScene();
		if (MovieScene)
		{
			int32 TotalEndFrame = EndFrame + 1;
			
			FFrameRate DisplayRate = MovieScene->GetDisplayRate();
			FFrameRate TickResolution = MovieScene->GetTickResolution();
			
			TRange<FFrameNumber> CurrentRange = MovieScene->GetPlaybackRange();
			
			FFrameNumber NeededStartInTicks = FFrameRate::TransformTime(
				FFrameTime(StartFrame),
				DisplayRate, 
				TickResolution
			).FloorToFrame();
			
			FFrameNumber NeededEndInTicks = FFrameRate::TransformTime(
				FFrameTime(TotalEndFrame),
				DisplayRate, 
				TickResolution
			).CeilToFrame();
			
			FFrameNumber FinalStartFrame = CurrentRange.GetLowerBoundValue();
			FFrameNumber FinalEndFrame = CurrentRange.GetUpperBoundValue();
			
			bool bRangeChanged = false;
			
			if (NeededStartInTicks < FinalStartFrame)
			{
				FinalStartFrame = NeededStartInTicks;
				bRangeChanged = true;
			}
			
			if (NeededEndInTicks > FinalEndFrame)
			{
				FinalEndFrame = NeededEndInTicks;
				bRangeChanged = true;
			}
			
			if (bRangeChanged)
			{
				TRange<FFrameNumber> NewRange = TRange<FFrameNumber>(FinalStartFrame, FinalEndFrame);
				MovieScene->SetPlaybackRange(NewRange);
			}
			
			// Set the view range to see the entire recording
			double ViewStart = FFrameRate::TransformTime(FFrameTime(StartFrame), DisplayRate, TickResolution).AsDecimal() / TickResolution.AsDecimal();
			double ViewEnd = FFrameRate::TransformTime(FFrameTime(EndFrame), DisplayRate, TickResolution).AsDecimal() / TickResolution.AsDecimal();
			
			Sequencer->SetViewRange(TRange<double>(ViewStart, ViewEnd), EViewRangeInterpolation::Immediate);
			
			// Just start playback - playhead is already at StartFrame from SetRecording()
			Sequencer->SetPlaybackStatus(EMovieScenePlayerStatus::Playing);
			
			UE_LOG(LogTemp, Warning, TEXT("===== Started Sequencer playback from frame %d ====="), StartFrame);
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
		return;
	}

	// Get the transform section
	UMovieScene3DTransformSection* TransformSection = Cast<UMovieScene3DTransformSection>(
		TransformTrack->FindSection(0));
	
	if (!TransformSection)
	{
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
		return;
	}

	int32 TotalKeysRemoved = 0;

	// Clear keyframes in the recording range for all channels
	for (FMovieSceneDoubleChannel* Channel : Channels)
	{
		if (Channel)
		{
			TArray<FKeyHandle> KeyHandlesToRemove;
			
			// Copy the times array to avoid invalidation issues
			TArray<FFrameNumber> KeyTimesCopy;
			TArrayView<const FFrameNumber> KeyTimes = Channel->GetTimes();
			KeyTimesCopy.Reserve(KeyTimes.Num());
			for (const FFrameNumber& Time : KeyTimes)
			{
				KeyTimesCopy.Add(Time);
			}
			
			// Find all key handles within the recording range
			for (int32 KeyIndex = 0; KeyIndex < KeyTimesCopy.Num(); ++KeyIndex)
			{
				const FFrameNumber& KeyTime = KeyTimesCopy[KeyIndex];
				if (KeyTime >= StartFrameInTicks && KeyTime <= EndFrameInTicks)
				{
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
		// Notify sequencer that data has changed
		if (TSharedPtr<ISequencer> Sequencer = ActiveSequencer.Pin())
		{
			Sequencer->NotifyMovieSceneDataChanged(EMovieSceneDataChangeType::TrackValueChanged);
		}
	}
}

void FCameraRecorderModule::SetRecording(bool bInIsRecording)
{
	bIsRecording = bInIsRecording;
	
	if (bInIsRecording)
	{
		// Reset tracking variables (click-to-start is now the only mode)
		bWaitingForViewportClick = true;
		bIsInCountdown = false;
		CountdownTimer = 0.0f;
		CountdownSecondsRemaining = 3;
		LastRecordedFrame = -1;
		RecordedFrames.Empty();
		CapturedCameraTransform.Reset();
		
		ULevelSequence* NewSequence = GetOrCreateLevelSequence();
		
		// If the sequence changed, invalidate the cached binding
		if (CurrentLevelSequence.Get() != NewSequence)
		{
			CachedCameraBinding.Invalidate();
		}
		
		CurrentLevelSequence = NewSequence;
		
		if (!CurrentLevelSequence.IsValid())
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to get or create Level Sequence!"));
			bIsRecording = false;
			bWaitingForViewportClick = false;
			return;
		}
		
		if (!GEditor)
		{
			UE_LOG(LogTemp, Error, TEXT("GEditor is null!"));
			bIsRecording = false;
			bWaitingForViewportClick = false;
			return;
		}

		ULevelEditorSubsystem* LevelEditorSubsystem = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
		if (!LevelEditorSubsystem)
		{
			UE_LOG(LogTemp, Error, TEXT("LevelEditorSubsystem is null!"));
			bIsRecording = false;
			bWaitingForViewportClick = false;
			return;
		}

		AActor* PilotActor = LevelEditorSubsystem->GetPilotLevelActor();
		if (!PilotActor)
		{
			UE_LOG(LogTemp, Error, TEXT("No piloted actor found! Please pilot a camera before recording."));
			bIsRecording = false;
			bWaitingForViewportClick = false;
			return;
		}

		ACineCameraActor* CineCam = Cast<ACineCameraActor>(PilotActor);
		if (!CineCam)
		{
			UE_LOG(LogTemp, Error, TEXT("Piloted actor is not a CineCameraActor!"));
			bIsRecording = false;
			bWaitingForViewportClick = false;
			return;
		}

		// Store the camera reference
		RecordingCamera = CineCam;

		// STEP 1: Capture the camera's transform FIRST, before doing anything that causes evaluation
		CapturedCameraTransform = CineCam->GetActorTransform();
		UE_LOG(LogTemp, Warning, TEXT("Captured camera transform: Loc=%s, Rot=%s"),
			*CapturedCameraTransform.GetValue().GetLocation().ToString(),
			*CapturedCameraTransform.GetValue().Rotator().ToString());

		// Get or create the camera binding
		FGuid CameraBinding = GetOrCreateCameraBinding(CineCam);
		if (!CameraBinding.IsValid())
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to get camera binding!"));
			bIsRecording = false;
			bWaitingForViewportClick = false;
			return;
		}

		// Clear existing keyframes in the recording range
		ClearExistingKeyframes(CameraBinding);
		
		// STEP 2: Disable the camera's transform track (this will cause a snap)
		UMovieScene* MovieScene = CurrentLevelSequence->GetMovieScene();
		if (MovieScene)
		{
			UMovieScene3DTransformTrack* TransformTrack = MovieScene->FindTrack<UMovieScene3DTransformTrack>(CameraBinding);
			if (TransformTrack)
			{
				TransformTrack->SetEvalDisabled(true);
				
				const TArray<UMovieSceneSection*>& Sections = TransformTrack->GetAllSections();
				for (UMovieSceneSection* Section : Sections)
				{
					if (Section)
					{
						Section->SetIsActive(false);
					}
				}
			}
		}
		
		// STEP 3: Move playhead to start frame and setup sequencer
		TSharedPtr<ISequencer> Sequencer = GetActiveSequencer();
		if (Sequencer.IsValid() && MovieScene)
		{
			ActiveSequencer = Sequencer;
			
			// Get frame rates
			FFrameRate DisplayRate = MovieScene->GetDisplayRate();
			FFrameRate TickResolution = MovieScene->GetTickResolution();
			
			// Set global time to StartFrame
			FFrameNumber StartFrameInTicks = FFrameRate::TransformTime(
				FFrameTime(StartFrame),
				DisplayRate,
				TickResolution
			).FloorToFrame();
			
			Sequencer->SetGlobalTime(FFrameTime(StartFrameInTicks));
			Sequencer->ForceEvaluate();
		}
		
		// STEP 4: Restore the captured transform (fixes any snap from disabling track or moving playhead)
		CineCam->SetActorTransform(CapturedCameraTransform.GetValue());
		UE_LOG(LogTemp, Warning, TEXT("Restored camera transform after setup"));
		
		UE_LOG(LogTemp, Warning, TEXT("===== Waiting for viewport click to start countdown... ====="));
		UE_LOG(LogTemp, Warning, TEXT("===== Will record from frame %d to %d (step: %d) ====="), StartFrame, EndFrame, FrameStep);
	}
	else
	{
		StopRecording();
	}
}

void FCameraRecorderModule::StopRecording()
{
	bIsRecording = false;
	bIsInCountdown = false;
	bWaitingForViewportClick = false;
	
	// Stop sequencer playback
	if (TSharedPtr<ISequencer> Sequencer = ActiveSequencer.Pin())
	{
		Sequencer->SetPlaybackStatus(EMovieScenePlayerStatus::Stopped);
	}
	
	// Re-enable the camera's transform track AND sections before writing keyframes
	if (CurrentLevelSequence.IsValid() && CachedCameraBinding.IsValid())
	{
		UMovieScene* MovieScene = CurrentLevelSequence->GetMovieScene();
		if (MovieScene)
		{
			UMovieScene3DTransformTrack* TransformTrack = MovieScene->FindTrack<UMovieScene3DTransformTrack>(CachedCameraBinding);
			if (TransformTrack)
			{
				TransformTrack->SetEvalDisabled(false);
				
				const TArray<UMovieSceneSection*>& Sections = TransformTrack->GetAllSections();
				for (UMovieSceneSection* Section : Sections)
				{
					if (Section)
					{
						Section->SetIsActive(true);
					}
				}
			}
		}
		
		// Write all the stored frames as keyframes
		if (RecordedFrames.Num() > 0)
		{
			ApplyRotationSnapCorrection();
			
			for (const FRecordedCameraFrame& Frame : RecordedFrames)
			{
				RecordCameraKeyframeWithRotation(Frame.Location, Frame.Rotation, Frame.FrameNumber);
			}
			
			UE_LOG(LogTemp, Warning, TEXT("Successfully wrote %d keyframes!"), RecordedFrames.Num());
			RecordedFrames.Empty();
		}
	}

	// Notify widget to update button state
	if (TSharedPtr<SCameraRecorderWidget> Widget = CameraRecorderWidget.Pin())
	{
		Widget->OnRecordingStopped();
	}
	
	UE_LOG(LogTemp, Warning, TEXT("===== Recording stopped at frame %d ====="), CurrentFrame);
	
	RecordingCamera.Reset();
	ActiveSequencer.Reset();
	LastRecordedFrame = -1;
}

ULevelSequence* FCameraRecorderModule::GetOrCreateLevelSequence()
{
	TSharedPtr<ISequencer> Sequencer = GetActiveSequencer();
	if (!Sequencer.IsValid())
	{
		return nullptr;
	}

	UMovieSceneSequence* FocusedSequence = Sequencer->GetFocusedMovieSceneSequence();
	if (!FocusedSequence)
	{
		return nullptr;
	}

	ULevelSequence* LevelSeq = Cast<ULevelSequence>(FocusedSequence);
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
		UMovieScene* MovieScene = CurrentLevelSequence->GetMovieScene();
		if (MovieScene)
		{
			TArray<UObject*, TInlineAllocator<1>> BoundObjects;
			CurrentLevelSequence->LocateBoundObjects(CachedCameraBinding, CineCam->GetWorld(), BoundObjects);
			
			for (UObject* BoundObject : BoundObjects)
			{
				if (BoundObject == CineCam)
				{
					return CachedCameraBinding;
				}
			}
			
			CachedCameraBinding.Invalidate();
		}
	}

	UMovieScene* MovieScene = CurrentLevelSequence->GetMovieScene();
	if (!MovieScene)
	{
		return FGuid();
	}

	// Search for existing possessables by comparing bound objects
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
				return CachedCameraBinding;
			}
		}
	}
	
	// Check if the camera is already bound as a spawnable
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
				return CachedCameraBinding;
			}
		}
	}
	
	// Try to find by name match
	FString CameraName = CineCam->GetActorLabel();
	for (int32 i = 0; i < MovieScene->GetPossessableCount(); ++i)
	{
		const FMovieScenePossessable& Possessable = MovieScene->GetPossessable(i);
		
		if (Possessable.GetName() == CameraName)
		{
			if (Possessable.GetPossessedObjectClass()->IsChildOf(ACineCameraActor::StaticClass()))
			{
				CachedCameraBinding = Possessable.GetGuid();
				return CachedCameraBinding;
			}
		}
	}
	
	// Camera not in sequence yet, add it as a new possessable
	CachedCameraBinding = MovieScene->AddPossessable(CineCam->GetActorLabel(), CineCam->GetClass());
	CurrentLevelSequence->BindPossessableObject(CachedCameraBinding, *CineCam, CineCam->GetWorld());
	
	return CachedCameraBinding;
}

void FCameraRecorderModule::RecordCameraKeyframe(ACineCameraActor* CineCam, int32 FrameNumber)
{
	// This function is kept for compatibility but not used anymore
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

	FGuid CameraBinding = CachedCameraBinding;
	if (!CameraBinding.IsValid())
	{
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
		return;
	}

	double PitchOffset = 0.0;
	double YawOffset = 0.0;
	double RollOffset = 0.0;

	FRotator PrevRotation = RecordedFrames[0].Rotation;

	for (int32 i = 1; i < RecordedFrames.Num(); ++i)
	{
		FRotator& CurrentRotation = RecordedFrames[i].Rotation;
		FRotator OriginalRotation = CurrentRotation;
		
		double DeltaPitch = CurrentRotation.Pitch - PrevRotation.Pitch;
		double DeltaYaw = CurrentRotation.Yaw - PrevRotation.Yaw;
		double DeltaRoll = CurrentRotation.Roll - PrevRotation.Roll;

		// Detect snaps
		if (DeltaPitch > 180.0)
		{
			PitchOffset -= 360.0;
		}
		else if (DeltaPitch < -180.0)
		{
			PitchOffset += 360.0;
		}
		
		if (DeltaYaw > 180.0)
		{
			YawOffset -= 360.0;
		}
		else if (DeltaYaw < -180.0)
		{
			YawOffset += 360.0;
		}
		
		if (DeltaRoll > 180.0)
		{
			RollOffset -= 360.0;
		}
		else if (DeltaRoll < -180.0)
		{
			RollOffset += 360.0;
		}

		// Apply accumulated offsets
		CurrentRotation.Pitch += PitchOffset;
		CurrentRotation.Yaw += YawOffset;
		CurrentRotation.Roll += RollOffset;

		PrevRotation = OriginalRotation;
	}
}

bool FCameraRecorderModule::HandleTicker(float DeltaTime)
{
	if (bIsRecording)
	{
		// Handle countdown state
		if (bIsInCountdown)
		{
			UpdateCountdown(DeltaTime);
			return true;
		}
		
		// Handle waiting for viewport click
		if (bWaitingForViewportClick)
		{
			// Check if user has clicked in the viewport (mouse button pressed)
			if (FSlateApplication::IsInitialized())
			{
				if (FSlateApplication::Get().GetPressedMouseButtons().Num() > 0)
				{
					UE_LOG(LogTemp, Warning, TEXT("===== Viewport clicked! ====="));
					StartCountdown();
				}
			}
			return true;
		}
		
		// Normal recording tick
		OnTick();
	}
	return true;
}

void FCameraRecorderModule::OnTick()
{
	if (!bIsRecording || bIsInCountdown || bWaitingForViewportClick)
	{
		return;
	}

	TSharedPtr<ISequencer> Sequencer = ActiveSequencer.Pin();
	if (!Sequencer.IsValid())
	{
		return;
	}

	// ADDED: Check if sequencer has stopped playing (reached the end)
	if (Sequencer->GetPlaybackStatus() != EMovieScenePlayerStatus::Playing)
	{
		UE_LOG(LogTemp, Warning, TEXT("Sequencer stopped playing, ending recording"));
		SetRecording(false);
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

	// Check if we've reached the end (this handles manual stop or if user changes end frame during recording)
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
					RecordedFrames.Add(FRecordedCameraFrame(EndFrame, CineCam->GetActorTransform()));
				}
			}
		}
		
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
			RecordedFrames.Add(FRecordedCameraFrame(CurrentFrame, CineCam->GetActorTransform()));
			LastRecordedFrame = CurrentFrame;
		}
	}
}

void FCameraRecorderModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

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

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FCameraRecorderModule, CameraRecorder)