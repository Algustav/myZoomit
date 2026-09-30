@echo off
setlocal
set "TASK_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%TASK_VSWHERE%" exit /b 1
for /f "usebackq tokens=*" %%i in (`"%TASK_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "TASK_VSROOT=%%i"
if not defined TASK_VSROOT exit /b 1
call "%TASK_VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
pushd "%~dp0.."
if not exist build mkdir build
rc /nologo /fo build\settings.res src\settings.rc
if errorlevel 1 exit /b 1
cl /nologo /utf-8 /std:c++20 /EHsc /W4 /O2 /MT /DUNICODE /D_UNICODE src\main.cpp build\settings.res /Fo:build\main.obj /Fe:build\MyZoomIt.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:src\app.manifest windowsapp.lib user32.lib shell32.lib dwmapi.lib wtsapi32.lib ole32.lib gdi32.lib comctl32.lib advapi32.lib > build\build.log 2>&1
set "TASK_RESULT=%ERRORLEVEL%"
type build\build.log
popd
exit /b %TASK_RESULT%
