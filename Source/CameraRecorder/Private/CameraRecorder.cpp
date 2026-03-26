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
	// Unregister tick delegate
	FTSTicker::GetCoreTicker().RemoveTicker(TickDelegateHandle);

	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);

	FCameraRecorderStyle::Shutdown();
	FCameraRecorderCommands::Unregister();

	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(CameraRecorderTabName);
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

void FCameraRecorderModule::SetRecording(bool bInIsRecording)
{
	bIsRecording = bInIsRecording;
	
	if (bInIsRecording)
	{
		CurrentFrame = StartFrame;
		CurrentLevelSequence = GetOrCreateLevelSequence();
		
		if (!CurrentLevelSequence.IsValid())
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to get or create Level Sequence!"));
			bIsRecording = false;
			return;
		}
		
		UE_LOG(LogTemp, Warning, TEXT("===== Recording started from frame %d to %d ====="), StartFrame, EndFrame);
		UE_LOG(LogTemp, Warning, TEXT("===== Recording to sequence: %s ====="), *CurrentLevelSequence->GetName());
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("===== Recording stopped at frame %d ====="), CurrentFrame);
		CurrentLevelSequence.Reset();
		RecordingCamera.Reset();
	}
}

void FCameraRecorderModule::StopRecording()
{
	bIsRecording = false;
	
	// Notify widget to update button state
	if (TSharedPtr<SCameraRecorderWidget> Widget = CameraRecorderWidget.Pin())
	{
		Widget->OnRecordingStopped();
	}
	
	UE_LOG(LogTemp, Warning, TEXT("===== Recording stopped at frame %d ====="), CurrentFrame);
	
	CurrentLevelSequence.Reset();
	RecordingCamera.Reset();
}

ULevelSequence* FCameraRecorderModule::GetOrCreateLevelSequence()
{
	// Try to get the asset editor subsystem and find an open sequence editor
	if (GEditor)
	{
		UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (AssetEditorSubsystem)
		{
			TArray<UObject*> EditedAssets = AssetEditorSubsystem->GetAllEditedAssets();
			
			for (UObject* Asset : EditedAssets)
			{
				if (ULevelSequence* LevelSeq = Cast<ULevelSequence>(Asset))
				{
					UE_LOG(LogTemp, Log, TEXT("Using currently open Level Sequence: %s"), *LevelSeq->GetName());
					return LevelSeq;
				}
			}
		}
	}
	
	UE_LOG(LogTemp, Warning, TEXT("No Level Sequence is currently open in Sequencer!"));
	UE_LOG(LogTemp, Warning, TEXT("Please open a Level Sequence in Sequencer before recording."));
	
	return nullptr;
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

	// Find existing binding or create new one
	FGuid CameraBinding;
	
	// Search for existing possessable
	for (int32 i = 0; i < MovieScene->GetPossessableCount(); ++i)
	{
		const FMovieScenePossessable& Possessable = MovieScene->GetPossessable(i);
		auto BoundObjects = CurrentLevelSequence->LocateBoundObjects(Possessable.GetGuid(), CineCam->GetWorld());
		
		for (UObject* BoundObject : BoundObjects)
		{
			if (BoundObject == CineCam)
			{
				CameraBinding = Possessable.GetGuid();
				break;
			}
		}
		
		if (CameraBinding.IsValid())
		{
			break;
		}
	}
	
	if (!CameraBinding.IsValid())
	{
		// Camera not in sequence yet, add it
		CameraBinding = MovieScene->AddPossessable(CineCam->GetActorLabel(), CineCam->GetClass());
		CurrentLevelSequence->BindPossessableObject(CameraBinding, *CineCam, CineCam->GetWorld());
		UE_LOG(LogTemp, Log, TEXT("Added camera %s to sequence"), *CineCam->GetActorLabel());
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

	// Add keyframes
	TArrayView<FMovieSceneDoubleChannel*> Channels = TransformSection->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
	
	if (Channels.Num() >= 6)
	{
		Channels[0]->AddLinearKey(FrameNum, Location.X); // Location X
		Channels[1]->AddLinearKey(FrameNum, Location.Y); // Location Y
		Channels[2]->AddLinearKey(FrameNum, Location.Z); // Location Z
		Channels[3]->AddLinearKey(FrameNum, Rotation.Roll);  // Rotation X
		Channels[4]->AddLinearKey(FrameNum, Rotation.Pitch); // Rotation Y
		Channels[5]->AddLinearKey(FrameNum, Rotation.Yaw);   // Rotation Z
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

	CurrentFrame++;

	// Check if we've reached the end frame
	if (EndFrame > 0 && CurrentFrame > EndFrame)
	{
		UE_LOG(LogTemp, Warning, TEXT("===== Reached end frame %d, stopping recording ====="), EndFrame);
		StopRecording();
		return;
	}

	// Only record on frame step intervals
	if ((CurrentFrame - StartFrame) % FrameStep != 0)
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

	const int32 KeyframeNumber = (CurrentFrame - StartFrame) / FrameStep;

	UE_LOG(LogTemp, Log, TEXT("[Keyframe %d | Frame %d] Recorded to Sequencer"),
		KeyframeNumber,
		CurrentFrame);
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FCameraRecorderModule, CameraRecorder)