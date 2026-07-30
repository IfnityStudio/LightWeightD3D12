# Desktop Retro Overlay

`LightD3D12DesktopRetroOverlay` captures the primary monitor with Windows
Graphics Capture, sends each capture frame through a shared GPU texture into
`LightD3D12`, and presents a retro post-process in a topmost click-through
window. The mouse and keyboard continue to reach the applications below the
overlay; controls are available from the tray, the compact control window, and
global hotkeys.

## Run

Build the `LightD3D12DesktopRetroOverlay` CMake target, then run the generated
executable. It starts enabled on the primary monitor.

For a non-visible startup/capture/render check, run it with `--smoke-test`.

| Shortcut | Action |
| --- | --- |
| Click the tray icon | Enable / disable the overlay |
| Right-click the tray icon | Presets and quit |
| `Ctrl + Alt + R` | Enable / disable the overlay, if available |
| `Ctrl + Alt + 1` | Simple CRTV preset |
| `Ctrl + Alt + 2` | Amber terminal preset |
| `Ctrl + Alt + 3` | Green phosphor preset |
| `Ctrl + Alt + 4` | PS2 Clean (480i) preset |
| `Ctrl + Alt + 5` | PS2 NewPixie CRT preset |
| `Ctrl + Alt + Up / Down` | Increase / decrease filter intensity |
| `Ctrl + Alt + Q` | Quit, if available |

## Requirements and limitations

- Windows 10 version 2004 or later is required. The app uses
  `WDA_EXCLUDEFROMCAPTURE` so the overlay is absent from its own capture; without
  that exclusion it would recursively capture itself.
- It targets the primary monitor and the GPU that drives it. It does not inject
  into applications or alter the physical monitor, so disabling it immediately
  restores the normal desktop.
- It performs one desktop copy and one full-screen post-process for every
  presented frame. Windowed and borderless-fullscreen games normally work, but
  expect a small GPU cost and at least one compositor/capture frame of latency;
  it is not suited to latency-critical competitive play.
- Protected video, the secure/UAC desktop, HDR/10-bit output, and exclusive
  fullscreen applications can be unavailable or fall outside what Desktop
  Graphics Capture can capture. In a hybrid-GPU laptop, run it on the GPU assigned to
  the primary monitor.

## Simple CRTV preset

The colour CRT preset is a clean-room HLSL treatment inspired by
[Simple CRT Shader](https://github.com/yunoda-3DCG/Simple-CRT-Shader): animated
scanlines, mild flicker, small tracking disturbances, RGB phosphor stripes and
subtle chromatic convergence. It intentionally does not use frame history or a
ghost pass, so text and window edges remain readable. Attribution and licensing
are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## PS2 Clean preset

`PS2 Clean (480i)` is a clean-room consumer-CRT treatment tuned for the look of
late PS2-era games: a 512×384 low-bandwidth signal reconstruction, gentle
component colour spread, broad 480-line beams, RGB phosphors and a convex face.
It is intentionally much stronger than the desktop-oriented Simple CRTV preset;
use `Ctrl + Alt + 1` to return to the sharper preset.

## PS2 NewPixie CRT preset

`PS2 NewPixie CRT` preserves the Clean preset and adds a separate multi-pass
path: phosphor persistence, separable horizontal/vertical bloom, channel beam
convergence and rolling scanlines. It is based on the NewPixie CRT pass layout
by Mattias Gustavsson, whose MIT/public-domain terms are recorded in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
