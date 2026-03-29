// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SSpinBox.h"

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
	FReply OnTestTickButtonClicked();
	FReply OnDetectCameraButtonClicked();

	void OnFrameStepChanged(int32 NewValue);
	void OnStartFrameChanged(int32 NewValue);
	void OnEndFrameChanged(int32 NewValue);
	void OnWarmupFramesChanged(int32 NewValue);

	FText GetCurrentFrameText() const;
	FText GetStatusText() const;

	bool bIsRecording = false;
	FCameraRecorderModule* Module = nullptr;
	
	TSharedPtr<SSpinBox<int32>> FrameStepSpinBox;
	TSharedPtr<SSpinBox<int32>> StartFrameSpinBox;
	TSharedPtr<SSpinBox<int32>> EndFrameSpinBox;
	TSharedPtr<SSpinBox<int32>> WarmupFramesSpinBox;
};