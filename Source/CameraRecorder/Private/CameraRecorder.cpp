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
		.SetDisplayName(LOCTEXT("FCameraRecorderTabTitle", "FlyCam Recorder"))
		.SetMenuType(ETabSpawnerMenuType::Hidden);

	TickDelegateHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FCameraRecorderModule::HandleTicker));
}

void FCameraRecorderModule::ShutdownModule()
{
	if (bIsRecording)
	{
		bIsRecording = false;
		bIsInCountdown = false;
		bWaitingForViewportClick = false;
	}
	
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
}

void FCameraRecorderModule::UpdateCountdown(float DeltaTime)
{
	if (!bIsInCountdown)
	{
		return;
	}
	
	CountdownTimer -= DeltaTime;
	int32 NewSecondsRemaining = FMath::CeilToInt(CountdownTimer);
	
	if (NewSecondsRemaining != CountdownSecondsRemaining)
	{
		CountdownSecondsRemaining = FMath::Max(0, NewSecondsRemaining);
	}
	
	if (CountdownTimer <= 0.0f)
	{
		bIsInCountdown = false;
		CountdownSecondsRemaining = 0;
		StartSequencerPlayback();
	}
}

void FCameraRecorderModule::StartSequencerPlayback()
{
	TSharedPtr<ISequencer> Sequencer = ActiveSequencer.Pin();
	if (!Sequencer.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("FlyCam Recorder: Failed to get Sequencer for playback!"));
		return;
	}

	if (CurrentLevelSequence.IsValid())
	{
		UMovieScene* MovieScene = CurrentLevelSequence->GetMovieScene();
		if (MovieScene)
		{
			// Sequencer's playback range is exclusive at the end, so we use EndFrame + 1
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
			
			// Only extend the playback range if necessary (never shrink it)
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
			
			// Zoom the sequencer view to show the entire recording range
			double ViewStart = FFrameRate::TransformTime(FFrameTime(StartFrame), DisplayRate, TickResolution).AsDecimal() / TickResolution.AsDecimal();
			double ViewEnd = FFrameRate::TransformTime(FFrameTime(EndFrame), DisplayRate, TickResolution).AsDecimal() / TickResolution.AsDecimal();
			
			Sequencer->SetViewRange(TRange<double>(ViewStart, ViewEnd), EViewRangeInterpolation::Immediate);
			
			// Playhead is already at StartFrame from SetRecording(), just start playback
			Sequencer->SetPlaybackStatus(EMovieScenePlayerStatus::Playing);
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

	UMovieScene3DTransformTrack* TransformTrack = MovieScene->FindTrack<UMovieScene3DTransformTrack>(CameraBinding);
	if (!TransformTrack)
	{
		return;
	}

	UMovieScene3DTransformSection* TransformSection = Cast<UMovieScene3DTransformSection>(
		TransformTrack->FindSection(0));
	
	if (!TransformSection)
	{
		return;
	}

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

	TArrayView<FMovieSceneDoubleChannel*> Channels = TransformSection->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
	
	if (Channels.Num() < 6)
	{
		return;
	}

	int32 TotalKeysRemoved = 0;

	// Remove all keyframes within the recording range for Location (XYZ) and Rotation (RPY)
	for (FMovieSceneDoubleChannel* Channel : Channels)
	{
		if (Channel)
		{
			TArray<FKeyHandle> KeyHandlesToRemove;
			
			// Copy times to avoid invalidation during iteration
			TArray<FFrameNumber> KeyTimesCopy;
			TArrayView<const FFrameNumber> KeyTimes = Channel->GetTimes();
			KeyTimesCopy.Reserve(KeyTimes.Num());
			for (const FFrameNumber& Time : KeyTimes)
			{
				KeyTimesCopy.Add(Time);
			}
			
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
			
			if (KeyHandlesToRemove.Num() > 0)
			{
				Channel->DeleteKeys(KeyHandlesToRemove);
				TotalKeysRemoved += KeyHandlesToRemove.Num();
			}
		}
	}

	if (TotalKeysRemoved > 0)
	{
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
		// Initialize recording state
		bWaitingForViewportClick = true;
		bIsInCountdown = false;
		CountdownTimer = 0.0f;
		CountdownSecondsRemaining = 3;
		LastRecordedFrame = -1;
		RecordedFrames.Empty();
		CapturedCameraTransform.Reset();
		
		ULevelSequence* NewSequence = GetOrCreateLevelSequence();
		
		if (CurrentLevelSequence.Get() != NewSequence)
		{
			CachedCameraBinding.Invalidate();
		}
		
		CurrentLevelSequence = NewSequence;
		
		if (!CurrentLevelSequence.IsValid())
		{
			UE_LOG(LogTemp, Error, TEXT("FlyCam Recorder: Failed to get or create Level Sequence!"));
			bIsRecording = false;
			bWaitingForViewportClick = false;
			return;
		}
		
		if (!GEditor)
		{
			UE_LOG(LogTemp, Error, TEXT("FlyCam Recorder: GEditor is null!"));
			bIsRecording = false;
			bWaitingForViewportClick = false;
			return;
		}

		ULevelEditorSubsystem* LevelEditorSubsystem = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
		if (!LevelEditorSubsystem)
		{
			UE_LOG(LogTemp, Error, TEXT("FlyCam Recorder: LevelEditorSubsystem is null!"));
			bIsRecording = false;
			bWaitingForViewportClick = false;
			return;
		}

		AActor* PilotActor = LevelEditorSubsystem->GetPilotLevelActor();
		if (!PilotActor)
		{
			UE_LOG(LogTemp, Error, TEXT("FlyCam Recorder: No piloted actor found! Please pilot a camera before recording."));
			bIsRecording = false;
			bWaitingForViewportClick = false;
			return;
		}

		ACineCameraActor* CineCam = Cast<ACineCameraActor>(PilotActor);
		if (!CineCam)
		{
			UE_LOG(LogTemp, Error, TEXT("FlyCam Recorder: Piloted actor is not a CineCameraActor!"));
			bIsRecording = false;
			bWaitingForViewportClick = false;
			return;
		}

		RecordingCamera = CineCam;

		// Capture transform before any operations that trigger Sequencer evaluation
		CapturedCameraTransform = CineCam->GetActorTransform();

		FGuid CameraBinding = GetOrCreateCameraBinding(CineCam);
		if (!CameraBinding.IsValid())
		{
			UE_LOG(LogTemp, Error, TEXT("FlyCam Recorder: Failed to get camera binding!"));
			bIsRecording = false;
			bWaitingForViewportClick = false;
			return;
		}

		ClearExistingKeyframes(CameraBinding);
		
		// Disable transform track to prevent Sequencer from controlling the camera during recording
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
		
		// Move playhead to StartFrame
		TSharedPtr<ISequencer> Sequencer = GetActiveSequencer();
		if (Sequencer.IsValid() && MovieScene)
		{
			ActiveSequencer = Sequencer;
			
			FFrameRate DisplayRate = MovieScene->GetDisplayRate();
			FFrameRate TickResolution = MovieScene->GetTickResolution();
			
			FFrameNumber StartFrameInTicks = FFrameRate::TransformTime(
				FFrameTime(StartFrame),
				DisplayRate,
				TickResolution
			).FloorToFrame();
			
			Sequencer->SetGlobalTime(FFrameTime(StartFrameInTicks));
			Sequencer->ForceEvaluate();
		}
		
		// Restore captured transform to prevent snapping from track disable or playhead movement
		CineCam->SetActorTransform(CapturedCameraTransform.GetValue());
		
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
	
	if (TSharedPtr<ISequencer> Sequencer = ActiveSequencer.Pin())
	{
		Sequencer->SetPlaybackStatus(EMovieScenePlayerStatus::Stopped);
	}
	
	// Re-enable transform track and write recorded keyframes
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
		
		if (RecordedFrames.Num() > 0)
		{
			ApplyRotationSnapCorrection();

			for (const FRecordedCameraFrame& Frame : RecordedFrames)
			{
				RecordCameraKeyframeWithRotation(Frame.Location, Frame.Rotation, Frame.FrameNumber);
			}

			RecordedFrames.Empty();
		}
	}

	if (TSharedPtr<SCameraRecorderWidget> Widget = CameraRecorderWidget.Pin())
	{
		Widget->OnRecordingStopped();
	}
	
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

	// Return cached binding if it's still valid for this camera
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

	// Search for existing possessable bindings
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
	
	// Check spawnable bindings
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
	
	// Try finding by name match
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
	
	// Camera not in sequence, add it as a new possessable
	CachedCameraBinding = MovieScene->AddPossessable(CineCam->GetActorLabel(), CineCam->GetClass());
	CurrentLevelSequence->BindPossessableObject(CachedCameraBinding, *CineCam, CineCam->GetWorld());
	
	return CachedCameraBinding;
}

void FCameraRecorderModule::RecordCameraKeyframe(ACineCameraActor* CineCam, int32 FrameNumber)
{
	// Deprecated - use RecordCameraKeyframeWithRotation instead
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

	UMovieScene3DTransformTrack* TransformTrack = MovieScene->FindTrack<UMovieScene3DTransformTrack>(CameraBinding);
	
	if (!TransformTrack)
	{
		TransformTrack = MovieScene->AddTrack<UMovieScene3DTransformTrack>(CameraBinding);
	}

	if (!TransformTrack)
	{
		return;
	}

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

	FFrameRate FrameRate = MovieScene->GetTickResolution();
	FFrameNumber FrameNum = FFrameRate::TransformTime(FFrameTime(FrameNumber), MovieScene->GetDisplayRate(), FrameRate).FloorToFrame();

	TArrayView<FMovieSceneDoubleChannel*> Channels = TransformSection->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
	
	// Channels: [0-2] = Location XYZ, [3-5] = Rotation RPY
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
	int32 CorrectionsApplied = 0;

	FRotator PrevRotation = RecordedFrames[0].Rotation;

	// Detect and fix rotation wrapping (e.g., 179° ? -179° should be 179° ? 181°)
	for (int32 i = 1; i < RecordedFrames.Num(); ++i)
	{
		FRotator& CurrentRotation = RecordedFrames[i].Rotation;
		FRotator OriginalRotation = CurrentRotation;
		
		double DeltaPitch = CurrentRotation.Pitch - PrevRotation.Pitch;
		double DeltaYaw = CurrentRotation.Yaw - PrevRotation.Yaw;
		double DeltaRoll = CurrentRotation.Roll - PrevRotation.Roll;

		bool bCorrected = false;
		
		// Detect 180° threshold crossings and apply offsets
		if (DeltaPitch > 180.0)
		{
			PitchOffset -= 360.0;
			bCorrected = true;
		}
		else if (DeltaPitch < -180.0)
		{
			PitchOffset += 360.0;
			bCorrected = true;
		}
		
		if (DeltaYaw > 180.0)
		{
			YawOffset -= 360.0;
			bCorrected = true;
		}
		else if (DeltaYaw < -180.0)
		{
			YawOffset += 360.0;
			bCorrected = true;
		}
		
		if (DeltaRoll > 180.0)
		{
			RollOffset -= 360.0;
			bCorrected = true;
		}
		else if (DeltaRoll < -180.0)
		{
			RollOffset += 360.0;
			bCorrected = true;
		}

		if (bCorrected)
		{
			CorrectionsApplied++;
		}

		CurrentRotation.Pitch += PitchOffset;
		CurrentRotation.Yaw += YawOffset;
		CurrentRotation.Roll += RollOffset;

		PrevRotation = OriginalRotation;
	}
	
	if (CorrectionsApplied > 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("FlyCam Recorder: Rotation snap correction fixed %d rotation snap(s) across %d frames"), CorrectionsApplied, RecordedFrames.Num());
	}
}

bool FCameraRecorderModule::HandleTicker(float DeltaTime)
{
	if (bIsRecording)
	{
		if (bIsInCountdown)
		{
			UpdateCountdown(DeltaTime);
			return true;
		}
		
		// Wait for user to click viewport to start countdown
		if (bWaitingForViewportClick)
		{
			if (FSlateApplication::IsInitialized())
			{
				if (FSlateApplication::Get().GetPressedMouseButtons().Num() > 0)
				{
					StartCountdown();
				}
			}
			return true;
		}
		
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

	// Stop recording if Sequencer has stopped playing (reached end of playback range)
	if (Sequencer->GetPlaybackStatus() != EMovieScenePlayerStatus::Playing)
	{
		SetRecording(false);
		return;
	}

	FQualifiedFrameTime CurrentTime = Sequencer->GetGlobalTime();
	UMovieScene* MovieScene = CurrentLevelSequence.IsValid() ? CurrentLevelSequence->GetMovieScene() : nullptr;
	
	if (!MovieScene)
	{
		return;
	}

	FFrameRate DisplayRate = MovieScene->GetDisplayRate();
	FFrameRate TickResolution = MovieScene->GetTickResolution();
	
	FFrameTime DisplayFrameTime = FFrameRate::TransformTime(CurrentTime.Time, TickResolution, DisplayRate);
	CurrentFrame = DisplayFrameTime.FloorToFrame().Value;

	// Stop if we've gone past the end frame
	if (CurrentFrame > EndFrame)
	{
		// Add final frame if enabled
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

	// Record this frame if it matches our frame step criteria
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