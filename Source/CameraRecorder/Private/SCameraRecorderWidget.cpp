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

	if (Module)
	{
		Module->SetStartFrame(0);
		Module->SetEndFrame(120);
		Module->SetFrameStep(20);
		Module->SetInterpMode(ECameraRecorderInterpMode::Auto);
		Module->SetKeyframeOnLastFrame(true);
	}

	bIsRecording = false;

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
				.Text(LOCTEXT("Title", "FlyCam Recorder"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
		]

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
					.Value(20)
					.OnValueChanged(this, &SCameraRecorderWidget::OnFrameStepChanged)
			]
		]

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
					.Text(LOCTEXT("SnapRotationCorrectionLabel", "Rotation Snap Correction:"))
					.MinDesiredWidth(100.f)
					.ToolTipText(LOCTEXT("SnapRotationCorrectionTooltip", "Automatically detect and fix rotation snaps/wraps"))
			]
			
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SAssignNew(SnapRotationCorrectionCheckBox, SCheckBox)
					.IsChecked(ECheckBoxState::Checked)
					.OnCheckStateChanged(this, &SCameraRecorderWidget::OnSnapRotationCorrectionChanged)
			]
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.f)
		[
			SNew(STextBlock)
				.Text(this, &SCameraRecorderWidget::GetStatusText)
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
				.ColorAndOpacity(this, &SCameraRecorderWidget::GetStatusColor)
		]

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

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.f)
		[
			SNew(SBox)
				.HeightOverride(45.f)
			[
				SNew(SButton)
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					.Text(this, &SCameraRecorderWidget::GetRecordButtonText)
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

	if (Module->IsWaitingForClick())
	{
		return LOCTEXT("StatusWaitingForClick", "Status: Click viewport to start countdown...");
	}
	else if (Module->IsInCountdown())
	{
		return FText::Format(
			LOCTEXT("StatusCountdown", "Status: Starting in {0}..."),
			FText::AsNumber(Module->GetCountdownSeconds())
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

void SCameraRecorderWidget::OnKeyframeOnLastFrameChanged(ECheckBoxState NewState)
{
	if (Module)
	{
		Module->SetKeyframeOnLastFrame(NewState == ECheckBoxState::Checked);
	}
}

void SCameraRecorderWidget::OnFrameStepChanged(int32 NewValue)
{
	if (Module)
	{
		Module->SetFrameStep(NewValue);
	}
}

void SCameraRecorderWidget::OnStartFrameChanged(int32 NewValue)
{
	if (Module)
	{
		Module->SetStartFrame(NewValue);
	}
}

void SCameraRecorderWidget::OnEndFrameChanged(int32 NewValue)
{
	if (Module)
	{
		Module->SetEndFrame(NewValue);
	}
}

FReply SCameraRecorderWidget::OnRecordButtonClicked()
{
	bIsRecording = !bIsRecording;

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

FSlateColor SCameraRecorderWidget::GetStatusColor() const
{
	if (!Module)
	{
		return FSlateColor(FLinearColor::White);
	}
	
	if (Module->IsWaitingForClick())
	{
		return FSlateColor(FLinearColor::Yellow);
	}
	else if (Module->IsInCountdown())
	{
		return FSlateColor(FLinearColor::Yellow);
	}
	else if (Module->IsRecording())
	{
		return FSlateColor(FLinearColor::Red);
	}
	else
	{
		return FSlateColor(FLinearColor::White);
	}
}

FText SCameraRecorderWidget::GetRecordButtonText() const
{
	return bIsRecording ?
		LOCTEXT("StopRecording", "Stop Recording") :
		LOCTEXT("Record", "Record");
}

#undef LOCTEXT_NAMESPACE