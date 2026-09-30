$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class MyZoomItWindow {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string name, string title);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr w, IntPtr l);
}
'@
$appWindow = [MyZoomItWindow]::FindWindow('MyZoomIt.Overlay.v1', 'MyZoomIt')
if ($appWindow -eq [IntPtr]::Zero) { exit 0 }
[uint32]$appProcessId = 0
[void][MyZoomItWindow]::GetWindowThreadProcessId($appWindow, [ref]$appProcessId)
$appProcess = Get-Process -Id $appProcessId
$expectedPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\build\MyZoomIt.exe'))
if ($appProcess.Path -ne $expectedPath) { throw 'Window belongs to a different checkout. Refusing to close it.' }
if (-not [MyZoomItWindow]::PostMessage($appWindow, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)) { throw 'Could not request application exit.' }
if (-not $appProcess.WaitForExit(5000)) { throw 'Application did not exit; not forcing termination.' }
Write-Output 'MyZoomIt exited normally.'
