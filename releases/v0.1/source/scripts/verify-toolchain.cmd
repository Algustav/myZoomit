@echo off
setlocal
set "TASK_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%TASK_VSWHERE%" exit /b 1
for /f "usebackq tokens=*" %%i in (`"%TASK_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "TASK_VSROOT=%%i"
if not defined TASK_VSROOT exit /b 1
call "%TASK_VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
pushd "%~dp0.."
if not exist .setup mkdir .setup
cl /nologo /std:c++20 /EHsc /W4 scripts\toolchain-smoke.cpp /Fo:.setup\toolchain-smoke.obj /Fe:.setup\toolchain-smoke.exe /link windowsapp.lib ole32.lib d3d11.lib dcomp.lib
if errorlevel 1 (popd & exit /b 1)
.setup\toolchain-smoke.exe
set "TASK_RESULT=%ERRORLEVEL%"
popd
exit /b %TASK_RESULT%
