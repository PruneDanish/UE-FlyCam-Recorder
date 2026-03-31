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

	// Reset module state when opening the window
	if (Module)
	{
		Module->SetStartFrame(0);
		Module->SetEndFrame(120);
		Module->SetFrameStep(10);  // Changed default to 10
		Module->SetWarmupFrames(30);
		Module->SetInterpMode(ECameraRecorderInterpMode::Auto);
		Module->SetKeyframeOnLastFrame(true);
	}

	bIsRecording = false;

	// Populate interpolation mode options
	InterpModeOptions.Add(MakeShareable(new ECameraRecorderInterpMode(ECameraRecorderInterpMode::Auto)));
	InterpModeOptions.Add(MakeShareable(new ECameraRecorderInterpMode(ECameraRecorderInterpMode::User)));
	InterpModeOptions.Add(MakeShareable(new ECameraRecorderInterpMode(ECameraRecorderInterpMode::Break)));
	InterpModeOptions.Add(MakeShareable(new ECameraRecorderInterpMode(ECameraRecorderInterpMode::Linear)));
	InterpModeOptions.Add(MakeShareable(new ECameraRecorderInterpMode(ECameraRecorderInterpMode::Constant)));

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
					.Value(10)  // Changed default to 10
					.OnValueChanged(this, &SCameraRecorderWidget::OnFrameStepChanged)
			]
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

		// Interpolation Mode Dropdown
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
					.Text(LOCTEXT("InterpModeLabel", "Interpolation:"))
					.MinDesiredWidth(100.f)
					.ToolTipText(LOCTEXT("InterpModeTooltip", "Animation curve interpolation mode for keyframes"))
			]
			
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SAssignNew(InterpModeComboBox, SComboBox<TSharedPtr<ECameraRecorderInterpMode>>)
					.OptionsSource(&InterpModeOptions)
					.OnGenerateWidget(this, &SCameraRecorderWidget::OnGenerateInterpWidget)
					.OnSelectionChanged(this, &SCameraRecorderWidget::OnInterpSelectionChanged)
					.InitiallySelectedItem(InterpModeOptions[0])
					[
						SNew(STextBlock)
							.Text(this, &SCameraRecorderWidget::GetCurrentInterpModeText)
					]
			]
		]

		// Keyframe on Last Frame Checkbox
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
					.Text(LOCTEXT("KeyframeOnLastFrameLabel", "Keyframe Last Frame:"))
					.MinDesiredWidth(100.f)
					.ToolTipText(LOCTEXT("KeyframeOnLastFrameTooltip", "Always add a keyframe on the final frame, regardless of frame step"))
			]
			
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SAssignNew(KeyframeOnLastFrameCheckBox, SCheckBox)
					.IsChecked(ECheckBoxState::Checked)
					.OnCheckStateChanged(this, &SCameraRecorderWidget::OnKeyframeOnLastFrameChanged)
			]
		]

		// Rotation Snap Correction Checkbox
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
					.Text(FText::FromString("Rotation Snap Correction:"))
					.MinDesiredWidth(100.f)
					.ToolTipText(FText::FromString("Automatically detect and fix rotation snaps/wraps"))
			]
			
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SAssignNew(SnapRotationCorrectionCheckBox, SCheckBox)
					.IsChecked(Module ? (Module->GetSnapRotationCorrection() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked) : ECheckBoxState::Unchecked)
					.OnCheckStateChanged(this, &SCameraRecorderWidget::OnSnapRotationCorrectionChanged)
					.ToolTipText(FText::FromString("Automatically detect and fix rotation snaps/wraps"))
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

		// Camera Display
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
					.Text(LOCTEXT("CameraLabel", "Camera:"))
					.MinDesiredWidth(100.f)
			]
			
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
					.Text(this, &SCameraRecorderWidget::GetCameraNameText)
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
			]
		]

		// Record Button
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.f)
		[
			SNew(SBox)
				.HeightOverride(45.f)  // 1.5x the default height (~40px -> 60px)
			[
				SNew(SButton)
					.HAlign(HAlign_Center)  // Center-align button content
					.VAlign(VAlign_Center)  // Vertically center the text
					.Text_Lambda([this]()
						{
							return bIsRecording ?
								LOCTEXT("StopRecording", "Stop Recording") :
								LOCTEXT("Record", "Record");
						})
					.OnClicked(this, &SCameraRecorderWidget::OnRecordButtonClicked)
			]
		]
	];
}

void SCameraRecorderWidget::OnRecordingStopped()
{
	bIsRecording = false;
}

FText SCameraRecorderWidget::GetCurrentFrameText() const
{
	if (Module && Module->IsRecording())
	{
		return FText::AsNumber(Module->GetCurrentFrame());
	}
	return FText::FromString(TEXT("-"));
}

FText SCameraRecorderWidget::GetCameraNameText() const
{
	if (!GEditor)
	{
		return LOCTEXT("NoCameraPiloted", "No camera piloted");
	}

	ULevelEditorSubsystem* LevelEditorSubsystem = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
	if (!LevelEditorSubsystem)
	{
		return LOCTEXT("NoCameraPiloted", "No camera piloted");
	}

	AActor* PilotActor = LevelEditorSubsystem->GetPilotLevelActor();
	if (!PilotActor)
	{
		return LOCTEXT("NoCameraPiloted", "No camera piloted");
	}

	ACineCameraActor* CineCam = Cast<ACineCameraActor>(PilotActor);
	if (!CineCam)
	{
		return FText::Format(LOCTEXT("NotACineCam", "{0} (not a CineCameraActor)"), 
			FText::FromString(PilotActor->GetActorLabel()));
	}

	return FText::FromString(CineCam->GetActorLabel());
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

void SCameraRecorderWidget::OnKeyframeOnLastFrameChanged(ECheckBoxState NewState)
{
	if (Module)
	{
		bool bChecked = (NewState == ECheckBoxState::Checked);
		Module->SetKeyframeOnLastFrame(bChecked);
		UE_LOG(LogTemp, Log, TEXT("Keyframe on Last Frame: %s"), bChecked ? TEXT("ON") : TEXT("OFF"));
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

TSharedRef<SWidget> SCameraRecorderWidget::OnGenerateInterpWidget(TSharedPtr<ECameraRecorderInterpMode> InItem)
{
	FText ItemText;
	
	if (InItem.IsValid())
	{
		switch (*InItem)
		{
			case ECameraRecorderInterpMode::Auto:
				ItemText = LOCTEXT("InterpAuto", "Auto (Smart)");
				break;
			case ECameraRecorderInterpMode::User:
				ItemText = LOCTEXT("InterpUser", "User");
				break;
			case ECameraRecorderInterpMode::Break:
				ItemText = LOCTEXT("InterpBreak", "Break");
				break;
			case ECameraRecorderInterpMode::Linear:
				ItemText = LOCTEXT("InterpLinear", "Linear");
				break;
			case ECameraRecorderInterpMode::Constant:
				ItemText = LOCTEXT("InterpConstant", "Constant");
				break;
		}
	}
	
	return SNew(STextBlock).Text(ItemText);
}

void SCameraRecorderWidget::OnInterpSelectionChanged(TSharedPtr<ECameraRecorderInterpMode> NewSelection, ESelectInfo::Type SelectInfo)
{
	if (Module && NewSelection.IsValid())
	{
		Module->SetInterpMode(*NewSelection);
		UE_LOG(LogTemp, Log, TEXT("Interpolation mode changed to: %d"), (int32)*NewSelection);
	}
}

FText SCameraRecorderWidget::GetCurrentInterpModeText() const
{
	if (!Module)
	{
		return LOCTEXT("InterpAuto", "Auto (Smart)");
	}
	
	switch (Module->GetInterpMode())
	{
		case ECameraRecorderInterpMode::Auto:
			return LOCTEXT("InterpAuto", "Auto (Smart)");
		case ECameraRecorderInterpMode::User:
			return LOCTEXT("InterpUser", "User");
		case ECameraRecorderInterpMode::Break:
			return LOCTEXT("InterpBreak", "Break");
		case ECameraRecorderInterpMode::Linear:
			return LOCTEXT("InterpLinear", "Linear");
		case ECameraRecorderInterpMode::Constant:
			return LOCTEXT("InterpConstant", "Constant");
		default:
			return LOCTEXT("InterpAuto", "Auto (Smart)");
	}
}

void SCameraRecorderWidget::OnSnapRotationCorrectionChanged(ECheckBoxState NewState)
{
	if (Module)
	{
		Module->SetSnapRotationCorrection(NewState == ECheckBoxState::Checked);
	}
}

#undef LOCTEXT_NAMESPACE