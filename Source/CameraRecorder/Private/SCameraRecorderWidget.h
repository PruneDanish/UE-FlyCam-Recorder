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

class SCameraRecorderWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCameraRecorderWidget) {}
		SLATE_ARGUMENT(FCameraRecorderModule*, Module)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Called by module when recording stops automatically */
	void OnRecordingStopped();

private:
	FReply OnRecordButtonClicked();

	void OnFrameStepChanged(int32 NewValue);
	void OnStartFrameChanged(int32 NewValue);
	void OnEndFrameChanged(int32 NewValue);
	void OnKeyframeOnLastFrameChanged(ECheckBoxState NewState);
	void OnSnapRotationCorrectionChanged(ECheckBoxState NewState);
	
	// Interpolation dropdown
	TSharedRef<SWidget> OnGenerateInterpWidget(TSharedPtr<ECameraRecorderInterpMode> InItem);
	void OnInterpSelectionChanged(TSharedPtr<ECameraRecorderInterpMode> NewSelection, ESelectInfo::Type SelectInfo);
	FText GetCurrentInterpModeText() const;

	FText GetCurrentFrameText() const;
	FText GetStatusText() const;
	FText GetCameraNameText() const;
	FSlateColor GetStatusColor() const;
	FText GetRecordButtonText() const;

	bool bIsRecording = false;
	FCameraRecorderModule* Module = nullptr;
	
	TSharedPtr<SSpinBox<int32>> FrameStepSpinBox;
	TSharedPtr<SSpinBox<int32>> StartFrameSpinBox;
	TSharedPtr<SSpinBox<int32>> EndFrameSpinBox;
	TSharedPtr<SCheckBox> KeyframeOnLastFrameCheckBox;
	TSharedPtr<SCheckBox> SnapRotationCorrectionCheckBox;
	
	// Interpolation dropdown data
	TArray<TSharedPtr<ECameraRecorderInterpMode>> InterpModeOptions;
	TSharedPtr<SComboBox<TSharedPtr<ECameraRecorderInterpMode>>> InterpModeComboBox;
};