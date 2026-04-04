# FlyCam Recorder

An Unreal Engine 5 plugin for recording piloted camera movements directly to Sequencer keyframes.

## Features

- **Click-to-Start Recording**: 3-second countdown after clicking viewport
- **Frame Step Control**: Record every Nth frame for optimized keyframe density
- **Flexible Frame Range**: Define custom start and end frames
- **Multiple Interpolation Modes**:
  - Auto (Smart) - Automatic tangent calculation
  - User - User-defined tangents
  - Break - Independent in/out tangents
  - Linear - Linear interpolation
  - Constant - Step/constant interpolation
- **Rotation Snap Correction**: Automatically detects and fixes 360° rotation wrapping
- **Keyframe Last Frame**: Option to always add a keyframe on the final frame
- **Real-time Feedback**: Live status display with countdown timer and current frame

## Requirements

- Unreal Engine 5.7 or later
- Visual Studio 2022

## Installation

### Marketplace Installation
*(Coming soon)*

### Manual Installation

1. Download the latest release from the [Releases](https://github.com/PruneDanish/UE-Camera-Recorder/releases) page
2. Extract the `CameraRecorder` folder to your project's `Plugins` directory:
3. Restart Unreal Editor
4. Enable the plugin in **Edit > Plugins > Project > Animation**

### Build from Source

1. Clone this repository into your project's `Plugins` folder:
2. Right-click your `.uproject` file and select **Generate Visual Studio project files**
3. Build the project in Visual Studio or Unreal Editor

## Usage

### Quick Start

1. **Open the FlyCam Recorder window**:
- Click the FlyCam Recorder button in the toolbar (next to Play button)

2. **Setup your camera**:
- Add a CineCameraActor to your level
- Right-click the camera in the Outliner and select **PilotCamera**
- Position your camera at the desired starting location

3. **Configure recording settings**:
- **Start Frame**: First frame to record (default: 0)
- **End Frame**: Last frame to record (default: 120)
- **Frame Step**: Recordevery Nth frame (default: 20)
- **Interpolation**: Choose your desired curve type
- **Keyframe Last Frame**: Ensure final frame is always keyframed
- **Rotation SnapCorrection**: Fix rotation wrapping issues

4. **Record**:
- Press **Record**
- Click anywherein the viewport to start the 3-second countdown
- Move your camera during playback
- Recording stopsautomatically at the end frame

5. **Review**:
- Your keyframes are now in the Sequencer timeline
- Adjust thecurve tangents as needed in the Curve Editor

### Best Practices

- **Frame Step**:Use larger values (10-20) for smooth camera moves, smaller values (1-5) for detailed movements
- **Rotation Snap Correction**: Keep enabled to avoid sudden360° jumps in rotation curves
- **Interpolation Modes**:
- Use**Auto** for most camera movements (smooth, natural curves)
- Use **Linear** for mechanical orrobotic camera moves
- Use **Constant** for snappy, stepped movements

### Workflow Tips

- Record the same camera multiple times to refine themovement
- Previous keyframes in the frame range are automatically cleared
- The sequence's playback range auto-extends if needed
- Use the **Current Frame** displayto monitor recording progress

## Troubleshooting

### "No piloted actor found!"
- Make sure you're piloting a camera (right-click camera> Pilot)
- The piloted actor must be a **CineCameraActor**

### Recording doesn't start
- Ensure you have a Level Sequence open in Sequencer
- Check that the camera is bound to the sequence

### Rotation jumps/snaps
- Enable **Rotation Snap Correction** in the plugin settings
- This automatically unwraps rotation values to prevent 360° wrapping

### Plugin won't package
- Close Visual Studio before packaging
- Ensure `.vs` folder is in your `.gitignore`

## Technical Details

### HowIt Works

1. **Pre-Recording**: Captures the camera's transform and disables the transform track
2. **Countdown**: 3-second delay after viewportclick
3. **Recording**: Samples camera position every Nth frame during Sequencer playback
4. **Post-Processing**: Applies rotation snap correction to prevent wrapping
5. **Keyframe Writing**: Writes all frames at oncewith the selected interpolation mode

### Rotation Unwrapping

The plugin stores rotation values as unwrapped Euler angles (can exceed ±180°) toprevent sudden jumps when crossing the 180° threshold. This is automatically corrected during post-processing if **Rotation Snap Correction** is enabled.

### Performance

- Uses frame stepping tominimize keyframe count
- Batch writes keyframes at the end of recording
- Minimal overhead duringrecording

## Contributing

Contributions are welcome! Please feel free to submit pull requests or open issues.

### DevelopmentSetup

1. Clone the repository
2. Open in Visual Studio 2022
3. Build the plugin
4. Test in a sampleUnreal project

## License

Copyright Epic Games, Inc. All Rights Reserved.

## Support

For bugs, feature requests, orquestions:
- Open an issue on [GitHub](https://github.com/PruneDanish/UE-Camera-Recorder/issues)
- Contact:[Your Contact Info]

## Changelog

### Version 1.0.0 (2026-04-04)
- Initial release
- Click-to-start recording with 3-second countdown
- Frame stepcontrol
- Multiple interpolation modes
- Rotation snap correction
- Customizable frame range
- Real-time status feedback

## Acknowledgments

Builtwith Unreal Engine 5.7