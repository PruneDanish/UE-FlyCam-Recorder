// Copyright Epic Games, Inc. All Rights Reserved.

#include "SCameraRecorderWidget.h"
#include "CameraRecorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "LevelEditorSubsystem.h"
#include "CineCameraActor.h"
#include "Editor.h"

#define LOCTEXT_NAMESPACE "SCameraRecorderWidget"

void SCameraRecorderWidget::Construct(const FArguments& InArgs)
{
	Module = InArgs._Module;

	// Reset module state when opening the window - only set values, don't call SetRecording
	if (Module)
	{
		Module->SetStartFrame(0);
		Module->SetEndFrame(120);
		Module->SetFrameStep(1);
		Module->SetWarmupFrames(30);
		// REMOVED: Module->SetRecording(false); - This line was causing the crash
	}

	bIsRecording = false;

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.f)
		[
			SNew(STextBlock)
				.Text(LOCTEXT("Title", "Camera Recorder"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
		]

		// Warmup Frames Input
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.f)
		[
			SNew(SHorizontalBox)
			
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.f, 0.f, 8.f, 0.f)
			[
				SNew(STextBlock)
					.Text(LOCTEXT("WarmupFramesLabel", "Warmup Frames:"))
					.MinDesiredWidth(100.f)
					.ToolTipText(LOCTEXT("WarmupFramesTooltip", "Number of frames to play before recording starts"))
			]
			
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SAssignNew(WarmupFramesSpinBox, SSpinBox<int32>)
					.MinValue(0)
					.MaxValue(1000)
					.Value(30)
					.OnValueChanged(this, &SCameraRecorderWidget::OnWarmupFramesChanged)
			]
		]

		// Start Frame Input
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.f)
		[
			SNew(SHorizontalBox)
			
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.f, 0.f, 8.f, 0.f)
			[
				SNew(STextBlock)
					.Text(LOCTEXT("StartFrameLabel", "Start Frame:"))
					.MinDesiredWidth(100.f)
			]
			
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SAssignNew(StartFrameSpinBox, SSpinBox<int32>)
					.MinValue(0)
					.MaxValue(10000)
					.Value(0)
					.OnValueChanged(this, &SCameraRecorderWidget::OnStartFrameChanged)
			]
		]

		// End Frame Input
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.f)
		[
			SNew(SHorizontalBox)
			
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.f, 0.f, 8.f, 0.f)
			[
				SNew(STextBlock)
					.Text(LOCTEXT("EndFrameLabel", "End Frame:"))
					.MinDesiredWidth(100.f)
			]
			
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SAssignNew(EndFrameSpinBox, SSpinBox<int32>)
					.MinValue(0)
					.MaxValue(10000)
					.Value(120)
					.OnValueChanged(this, &SCameraRecorderWidget::OnEndFrameChanged)
			]
		]

		// Frame Step Input
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.f)
		[
			SNew(SHorizontalBox)
			
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.f, 0.f, 8.f, 0.f)
			[
				SNew(STextBlock)
					.Text(LOCTEXT("FrameStepLabel", "Frame Step:"))
					.MinDesiredWidth(100.f)
			]
			
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SAssignNew(FrameStepSpinBox, SSpinBox<int32>)
					.MinValue(1)
					.MaxValue(100)
					.Value(1)
					.OnValueChanged(this, &SCameraRecorderWidget::OnFrameStepChanged)
			]
		]

		// Status Display
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.f)
		[
			SNew(STextBlock)
				.Text(this, &SCameraRecorderWidget::GetStatusText)
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
				.ColorAndOpacity_Lambda([this]()
					{
						if (!Module) return FSlateColor(FLinearColor::White);
						
						if (Module->IsInWarmup())
							return FSlateColor(FLinearColor::Yellow);
						else if (Module->IsRecording())
							return FSlateColor(FLinearColor::Red);
						else
							return FSlateColor(FLinearColor::White);
					})
		]

		// Current Frame Display
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.f)
		[
			SNew(SHorizontalBox)
			
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.f, 0.f, 8.f, 0.f)
			[
				SNew(STextBlock)
					.Text(LOCTEXT("CurrentFrameLabel", "Current Frame:"))
					.MinDesiredWidth(100.f)
			]
			
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
					.Text(this, &SCameraRecorderWidget::GetCurrentFrameText)
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
			]
		]

		// Record Button
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.f)
		[
			SNew(SButton)
				.Text_Lambda([this]()
					{
						return bIsRecording ?
							LOCTEXT("StopRecording", "Stop Recording") :
							LOCTEXT("Record", "Record");
					})
				.OnClicked(this, &SCameraRecorderWidget::OnRecordButtonClicked)
		]

		// Test Tick Button
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.f)
		[
			SNew(SButton)
				.Text(LOCTEXT("TestTick", "Test Tick Function"))
				.OnClicked(this, &SCameraRecorderWidget::OnTestTickButtonClicked)
		]

		// Detect Camera Button
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.f)
		[
			SNew(SButton)
				.Text(LOCTEXT("DetectCamera", "Detect Camera"))
				.OnClicked(this, &SCameraRecorderWidget::OnDetectCameraButtonClicked)
		]
	];
}

void SCameraRecorderWidget::OnRecordingStopped()
{
	bIsRecording = false;
}

FText SCameraRecorderWidget::GetCurrentFrameText() const
{
	if (Module)
	{
		return FText::AsNumber(Module->GetCurrentFrame());
	}
	return FText::FromString(TEXT("0"));
}

FText SCameraRecorderWidget::GetStatusText() const
{
	if (!Module)
	{
		return LOCTEXT("StatusIdle", "Status: Idle");
	}

	if (Module->IsInWarmup())
	{
		return FText::Format(
			LOCTEXT("StatusWarmup", "Status: Warming up... ({0}/{1})"),
			FText::AsNumber(Module->GetCurrentFrame() - (Module->GetStartFrame() - Module->GetWarmupFrames())),
			FText::AsNumber(Module->GetWarmupFrames())
		);
	}
	else if (Module->IsRecording())
	{
		return LOCTEXT("StatusRecording", "Status: Recording");
	}
	else
	{
		return LOCTEXT("StatusIdle", "Status: Idle");
	}
}

void SCameraRecorderWidget::OnWarmupFramesChanged(int32 NewValue)
{
	if (Module)
	{
		Module->SetWarmupFrames(NewValue);
		UE_LOG(LogTemp, Log, TEXT("Warmup Frames changed to: %d"), NewValue);
	}
}

void SCameraRecorderWidget::OnFrameStepChanged(int32 NewValue)
{
	if (Module)
	{
		Module->SetFrameStep(NewValue);
		UE_LOG(LogTemp, Log, TEXT("Frame Step changed to: %d"), NewValue);
	}
}

void SCameraRecorderWidget::OnStartFrameChanged(int32 NewValue)
{
	if (Module)
	{
		Module->SetStartFrame(NewValue);
		UE_LOG(LogTemp, Log, TEXT("Start Frame changed to: %d"), NewValue);
	}
}

void SCameraRecorderWidget::OnEndFrameChanged(int32 NewValue)
{
	if (Module)
	{
		Module->SetEndFrame(NewValue);
		UE_LOG(LogTemp, Log, TEXT("End Frame changed to: %d"), NewValue);
	}
}

FReply SCameraRecorderWidget::OnRecordButtonClicked()
{
	bIsRecording = !bIsRecording;

	// Notify the module
	if (Module)
	{
		Module->SetRecording(bIsRecording);
	}

	return FReply::Handled();
}

FReply SCameraRecorderWidget::OnTestTickButtonClicked()
{
	UE_LOG(LogTemp, Warning, TEXT("Test Tick Function clicked"));
	UE_LOG(LogTemp, Warning, TEXT("bIsRecording: %s"), bIsRecording ? TEXT("TRUE") : TEXT("FALSE"));
	if (Module)
	{
		UE_LOG(LogTemp, Warning, TEXT("Module bIsRecording: %s"), Module->IsRecording() ? TEXT("TRUE") : TEXT("FALSE"));
		UE_LOG(LogTemp, Warning, TEXT("Warmup Frames: %d"), Module->GetWarmupFrames());
		UE_LOG(LogTemp, Warning, TEXT("Frame Step: %d"), Module->GetFrameStep());
		UE_LOG(LogTemp, Warning, TEXT("Frame Range: %d - %d"), Module->GetStartFrame(), Module->GetEndFrame());
	}
	return FReply::Handled();
}

FReply SCameraRecorderWidget::OnDetectCameraButtonClicked()
{
	if (!GEditor)
	{
		UE_LOG(LogTemp, Warning, TEXT("GEditor is null"));
		return FReply::Handled();
	}

	ULevelEditorSubsystem* LevelEditorSubsystem =
		GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();

	if (!LevelEditorSubsystem)
	{
		UE_LOG(LogTemp, Warning, TEXT("LevelEditorSubsystem is null"));
		return FReply::Handled();
	}

	AActor* PilotActor = LevelEditorSubsystem->GetPilotLevelActor();

	if (!PilotActor)
	{
		UE_LOG(LogTemp, Warning, TEXT("No piloted actor found"));
		return FReply::Handled();
	}

	ACineCameraActor* CineCam = Cast<ACineCameraActor>(PilotActor);

	if (!CineCam)
	{
		UE_LOG(LogTemp, Warning, TEXT("Piloted actor is not a CineCameraActor: %s"), *PilotActor->GetName());
		return FReply::Handled();
	}

	UE_LOG(LogTemp, Warning, TEXT("Detected CineCameraActor: %s"), *CineCam->GetName());

	const FTransform CameraTransform = CineCam->GetActorTransform();
	const FVector Location = CameraTransform.GetLocation();
	const FRotator Rotation = CameraTransform.Rotator();

	UE_LOG(LogTemp, Warning, TEXT("Cam Loc: X=%.2f Y=%.2f Z=%.2f | Rot: P=%.2f Y=%.2f R=%.2f"),
		Location.X, Location.Y, Location.Z,
		Rotation.Pitch, Rotation.Yaw, Rotation.Roll);

	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE