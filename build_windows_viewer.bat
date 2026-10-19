@echo off
setlocal
set "REPO_ROOT=%~dp0"
set "OUT_DIR=E:\Desktop\SwitchU\- Switch Frame Tap -\build\Switch Frame Tap (Windows)"

echo [switch-frame-tap] Windows viewer build (not the SD card payload)
echo [switch-frame-tap] Source: %REPO_ROOT%
echo [switch-frame-tap] Output: %OUT_DIR%
echo [switch-frame-tap] First run downloads Zig/SDL2/libusb/FFmpeg into ref\win (~200 MB).
echo [switch-frame-tap] For the Switch SD build, use build_local.bat instead.
echo.

powershell -NoProfile -ExecutionPolicy Bypass -File "%REPO_ROOT%scripts\build-windows-viewer.ps1" -OutDir "%OUT_DIR%" %*

if %ERRORLEVEL% neq 0 (
    echo.
    echo [switch-frame-tap] Windows viewer build FAILED!
    echo [switch-frame-tap] Check network access, or re-run with -ForceDeps to re-download.
    pause
    exit /b %ERRORLEVEL%
)

echo.
echo [switch-frame-tap] Build Successful!
echo [switch-frame-tap] Open: %OUT_DIR%
pause
