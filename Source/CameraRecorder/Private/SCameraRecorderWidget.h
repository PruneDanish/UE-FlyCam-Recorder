// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SCheckBox.h"
#include "CameraRecorder.h"

class FCameraRecorderModule;

/** UI widget for FlyCam Recorder controls */
class SCameraRecorderWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCameraRecorderWidget) {}
		SLATE_ARGUMENT(FCameraRecorderModule*, Module)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	void OnRecordingStopped();

private:
	// Button and input callbacks
	FReply OnRecordButtonClicked();
	void OnFrameStepChanged(int32 NewValue);
	void OnStartFrameChanged(int32 NewValue);
	void OnEndFrameChanged(int32 NewValue);
	void OnKeyframeOnLastFrameChanged(ECheckBoxState NewState);
	void OnSnapRotationCorrectionChanged(ECheckBoxState NewState);
	
	// Interpolation dropdown callbacks
	TSharedRef<SWidget> OnGenerateInterpWidget(TSharedPtr<ECameraRecorderInterpMode> InItem);
	void OnInterpSelectionChanged(TSharedPtr<ECameraRecorderInterpMode> NewSelection, ESelectInfo::Type SelectInfo);
	FText GetCurrentInterpModeText() const;

	// Dynamic text/color providers
	FText GetCurrentFrameText() const;
	FText GetStatusText() const;
	FText GetCameraNameText() const;
	FSlateColor GetStatusColor() const;
	FText GetRecordButtonText() const;

	bool bIsRecording = false;
	FCameraRecorderModule* Module = nullptr;
	
	// Widget references
	TSharedPtr<SSpinBox<int32>> FrameStepSpinBox;
	TSharedPtr<SSpinBox<int32>> StartFrameSpinBox;
	TSharedPtr<SSpinBox<int32>> EndFrameSpinBox;
	TSharedPtr<SCheckBox> KeyframeOnLastFrameCheckBox;
	TSharedPtr<SCheckBox> SnapRotationCorrectionCheckBox;
	
	TArray<TSharedPtr<ECameraRecorderInterpMode>> InterpModeOptions;
	TSharedPtr<SComboBox<TSharedPtr<ECameraRecorderInterpMode>>> InterpModeComboBox;
};