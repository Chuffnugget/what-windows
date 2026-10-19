# Regenerating WHAT branding

`qml/components/AnimatedLogo.qml` is the approved animation source. The exporter renders that component directly; it does not contain a second animation implementation. `assets/logo.svg` and `logo-roof.svg` describe the same resting geometry.

Requires the Qt QML runtime with QtQuick.Shapes, Python with Pillow, and a configured CMake build. Run these steps from the Windows repository, substituting your Qt/Python executables and build directory as needed:

```powershell
New-Item -ItemType Directory -Force build/logo-export/frames | Out-Null
Push-Location build/logo-export
qml ../../tools/export-logo.qml
Pop-Location
python tools/package-logo.py build/logo-export/frames --integration ../what-integration
cmake --build build/Desktop_Qt_6_11_2_MSVC2022_64bit-Debug --target icon_builder
& ./build/Desktop_Qt_6_11_2_MSVC2022_64bit-Debug/icon_builder.exe assets/app-icon.png assets
```

Use display scale 100% for the export process (`$env:QT_SCALE_FACTOR='1'`). The exporter captures 76 transparent frames at twice the design resolution. Packaging verifies matching start/end images and a 1,680 ms loop (1,500 ms animation plus 180 ms pause). Identical frames may be merged by GIF encoding while preserving duration. GIF supports binary transparency; PNG and the application retain smooth alpha edges.

Generated assets:

- `assets/app-icon.png`: the current resting logo, used by About and installer artwork.
- `assets/app-icon.ico`: 16, 24, 32, 48, 64, 128 and 256 px frames for the executable, title bar, taskbar and tray.
- `assets/Logo44.png`, `Logo150.png`, `StoreLogo.png`: package/notification branding.
- `assets/what-logo.gif`: current animation for the Windows and umbrella READMEs.
- Integration `assets/what-logo.gif`: identical animation for its README.
- Integration `custom_components/what/brand/{icon,icon@2x,logo,logo@2x}.png`: local Home Assistant branding.

Rebuild WHAT after regeneration to embed the updated resources. The release build also regenerates the ICO and package PNGs from `app-icon.png`. Commit assets in each respective repository; updating source files does not update an already installed Home Assistant integration or an already published release.
