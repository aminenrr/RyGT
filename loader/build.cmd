@echo off
rem Builds RyGTLauncher.exe (the launcher for GT Sport and GT7, FEAT-GTS-290) with the C# compiler that ships
rem with Windows' .NET Framework 4.x.
rem Usage: build.cmd [output folder] [Release|Diagnostics]
setlocal
set OUT=%~1
if "%OUT%"=="" set OUT=%~dp0
set CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe
if not exist "%CSC%" set CSC=%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe
set SYMBOLS=
if /i "%~2"=="Diagnostics" set SYMBOLS=/debug:full /pdb:"%OUT%\RyGTLauncher.pdb"
"%CSC%" /nologo /target:winexe /optimize+ %SYMBOLS% /win32icon:"%~dp0ryGT.ico" /resource:"%~dp0ryGT.ico",ryGT.ico ^
    /resource:"%~dp0launcher\background.png",gts-background.png /resource:"%~dp0launcher-gt7\background.png",gt7-background.png ^
    /resource:"%~dp0launcher-gt7\logo.png",gt7-logo.png /resource:"%~dp0launcher\logo.png",gts-logo.png /out:"%OUT%\RyGTLauncher.exe" ^
    /r:System.dll /r:System.Core.dll /r:System.Drawing.dll /r:System.Windows.Forms.dll /r:System.Web.Extensions.dll /r:System.Xml.dll ^
    /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll /r:System.Security.dll ^
    "%~dp0RenderResolution.cs" "%~dp0VulkanDevices.cs" "%~dp0Version.cs" "%~dp0GameTarget.cs" "%~dp0RyGTLoader.cs" "%~dp0GtsPages.cs" "%~dp0Gt7Pages.cs" ^
    "%~dp0ControlsForm.cs" "%~dp0PortsForm.cs" "%~dp0Logging.cs" "%~dp0WheelForm.cs" "%~dp0FeelForm.cs" "%~dp0CustomEnvironment.cs" "%~dp0AdvancedPage.cs" ^
    "%~dp0Updater.cs" "%~dp0ObsoleteSettings.cs" ^
    "%~dp0GT7Setup.cs" "%~dp0Bindings.cs" "%~dp0ControlsPage.cs" "%~dp0Wheel.cs" "%~dp0DlssDownload.cs" "%~dp0Online.cs" "%~dp0Trophies.cs" ^
    /resource:"%~dp0trophy_assets\bronze.png",trophy.bronze.png /resource:"%~dp0trophy_assets\silver.png",trophy.silver.png /resource:"%~dp0trophy_assets\gold.png",trophy.gold.png /resource:"%~dp0trophy_assets\platinum.png",trophy.platinum.png /resource:"%~dp0trophy_assets\trophy.wav",trophy.trophy.wav /resource:"%~dp0trophy_assets\platinum.wav",trophy.platinum.wav /resource:"%~dp0trophy_assets\font.ttf",trophy.font.ttf /resource:"%~dp0trophy_assets\font-LICENSE.txt",trophy.font-LICENSE.txt
if errorlevel 1 exit /b 1
rem PRT-TROPHYEXTRACT (fork, pr-tests): RyGTTrophyExtract.exe, extracts trophy00.trp with the player's own key.
"%CSC%" /nologo /target:winexe /optimize+ /win32icon:"%~dp0ryGT.ico" /out:"%OUT%\RyGTTrophyExtract.exe" ^
    /r:System.dll /r:System.Core.dll /r:System.Drawing.dll /r:System.Windows.Forms.dll /r:System.Security.dll "%~dp0TrophyExtract.cs"
