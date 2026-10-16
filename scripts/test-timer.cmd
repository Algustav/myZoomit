@echo off
setlocal
set "TASK_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%TASK_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "TASK_VSROOT=%%i"
if not defined TASK_VSROOT exit /b 1
call "%TASK_VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
pushd "%~dp0.."
if not exist build mkdir build
rc /nologo /fo build\timer-test.res src\settings.rc
if errorlevel 1 exit /b 1
cl /nologo /utf-8 /std:c++20 /EHsc /W4 /MT /DUNICODE /D_UNICODE tests\timer_test.cpp build\timer-test.res /Fo:build\timer-test.obj /Fe:build\timer-test.exe /link /MANIFEST:EMBED /MANIFESTINPUT:src/app.manifest user32.lib gdi32.lib gdiplus.lib comctl32.lib
if errorlevel 1 exit /b 1
build\timer-test.exe
set "TASK_RESULT=%ERRORLEVEL%"
popd
exit /b %TASK_RESULT%
