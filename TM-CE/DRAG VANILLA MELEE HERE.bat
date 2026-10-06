@echo off
setlocal

REM set directory to location of this file. Needed when drag n dropping across directories.
cd /d %~dp0

set ISO="%~1"

if %ISO%=="" (
    echo Please drag a v1.02 NTSC melee iso on top of 'DRAG VANILLA MELEE HERE.bat'
    goto end
) else (
    echo iso: %ISO%
)

xdelta3 -f -d -s %ISO% patch.xdelta TM-CE-LandingLab.iso || ( echo ERROR: The ISO is not a valid v1.02 NTSC melee iso & goto end )
echo TM-CE-LandingLab.iso has been successfully created!

:end

REM pause if not run from command line
echo %CMDCMDLINE% | findstr /C:"/c">nul && pause
