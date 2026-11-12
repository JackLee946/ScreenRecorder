# ScreenRecorder

Qt5 tray-resident screen recorder for Windows (ffmpeg gdigrab + libx264, MP4 output).

- Fullscreen or region recording, 10/15/25/30/60 fps
- Double-click the tray icon to start/stop
- Needs ffmpeg.exe next to the exe, in PATH, or set from the tray menu

## Build

    qmake ScreenRecorder.pro && nmake    (or open the .pro in Qt Creator)
