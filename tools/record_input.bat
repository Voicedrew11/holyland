@echo off
setlocal
rem Play King's Field IV as usual while your input is recorded, so the developers can replay the run exactly
rem (headless: tools/record_input.py replay). The same as "tools/record_input.py record", for Windows without Python.
rem
rem   record_input.bat [kf4.exe] [disc image] [more kf4 options]
rem
rem   kf4.exe     the program (default %KF4_EXE%, else kf4.exe beside this file, else out\windows-clang\kf4.exe)
rem   disc image  default %KF4_DISC% (leave both out to use what settings.json says)
rem
rem 1. Run it. The game starts in its window with your settings and loads your newest quick save by itself:
rem    do NOT press the quick load key; wait for it.
rem 2. Play to what you want to show. Press F8 when it is on the screen (the vblank goes into the recording).
rem 3. Close the game. The recording is complete when the game exits.
rem Mouse look, the run key, twin-stick and the weapon swap key are recorded: play normally. Avoid the quick load
rem key, save states and changing settings in the overlay mid-run (those are not replayed); opening the overlay or
rem the full map is fine.
rem
rem It goes to <data folder>\captures\input-DATE-TIME\ (beside F8's and F9's captures; the data folder is
rem %KF4_DATA_DIR%, else %APPDATA%\darker-reality; %KF4_REC% names another folder):
rem   input.pad  the recording        start\  your settings, quick saves, automap, hands, weapon sets and saves as
rem   info.txt   program, quick save, clock    they were at the start (copied; nothing in the data folder changes)
rem Send the whole folder (and your F8 pictures from captures\). It holds your saves: it is not for the repository.

set "EXE=%KF4_EXE%"
if not "%~1"=="" (set "EXE=%~f1" & shift)
if not defined EXE if exist "%~dp0kf4.exe" set "EXE=%~dp0kf4.exe"
if not defined EXE set "EXE=%~dp0..\out\windows-clang\kf4.exe"
if not exist "%EXE%" (echo kf4.exe not found: %EXE% & pause & exit /b 1)
set "DISC=%KF4_DISC%"
if not "%~1"=="" (set "DISC=%~f1" & shift)
set "EXTRA="
:more
if "%~1"=="" goto done
set "EXTRA=%EXTRA% %1"
shift
goto more
:done
set "DISCARG="
if defined DISC set DISCARG=--disc "%DISC%"

set "DATA=%KF4_DATA_DIR%"
if not defined DATA set "DATA=%APPDATA%\darker-reality"
set "CLOCK=2026-01-01T12:00:00"
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "STAMP=%%i"
set "REC=%KF4_REC%"
if not defined REC set "REC=%DATA%\captures\input-%STAMP%"

set "QUICK="
for /f "delims=" %%f in ('dir /b /a-d /o-d "%DATA%\quicksaves\*.kf4quick" 2^>nul') do if not defined QUICK set "QUICK=%%f"
if not defined QUICK (echo No quick save found in %DATA%\quicksaves & pause & exit /b 1)

mkdir "%REC%\start" >nul 2>&1
if exist "%DATA%\settings.json" copy /y "%DATA%\settings.json" "%REC%\start\" >nul
if exist "%DATA%\overlay.ini" copy /y "%DATA%\overlay.ini" "%REC%\start\" >nul
for %%d in (quicksaves automap hands weapon_sets saves) do if exist "%DATA%\%%d" robocopy "%DATA%\%%d" "%REC%\start\%%d" /e /njh /njs /nfl /ndl /np >nul
(
echo exe=%EXE%
echo quick=%QUICK%
echo clock=%CLOCK%
echo started=%STAMP%
echo args=%EXTRA%
) > "%REC%\info.txt"

echo Recording to %REC%
echo Loading quick save %QUICK% ...
"%EXE%" %DISCARG% --clock %CLOCK% --quick-load "%REC%\start\quicksaves\%QUICK%" --record-input "%REC%\input.pad" %EXTRA%
if errorlevel 1 (echo. & echo kf4 exited with code %errorlevel% & pause & exit /b 1)
echo Recording saved in %REC%
timeout /t 5 >nul 2>&1
