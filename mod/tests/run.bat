@echo off
rem Glint Spotter tests. Five of them, all compiling real source files rather
rem than copies, with the log and the settings stubbed out.
rem
rem   parse_paths     the save-path parser, against the paths the game was
rem                   caught opening and the ones it must refuse
rem   pinstore_flow   the pin file through the sequences a player produces,
rem                   including the two that lost pins in 1.1.0 and 1.1.1
rem   flash_flag      the flash flag through a save load, which frees the
rem                   object it is read from; 1.1.20 read the reused block
rem   pad_reports     DualSense and DualShock 4 reports into XInput buttons,
rem                   written with no such pad to test on
rem   stale_actors    the entity set reading components a load has freed, which
rem                   is where 16 September's access violations came from
rem
rem Run it by full quoted path from PowerShell; the space in the repo path
rem breaks a bare "cmd /c run.bat". Anything it builds lands in build\.
setlocal enabledelayedexpansion
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set "HERE=%~dp0"
set "OUT=%HERE%build"

call "%VCVARS%" >nul 2>&1
if errorlevel 1 (
  echo vcvars64.bat not found at "%VCVARS%"
  exit /b 1
)
if not exist "%OUT%" mkdir "%OUT%"
pushd "%OUT%"

set FAILED=0
for %%T in (parse_paths pinstore_flow flash_flag pad_reports stale_actors) do (
  cl /nologo /EHsc /std:c++17 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS ^
     /I"%HERE%..\src" "%HERE%%%T.cpp" /Fe:%%T.exe
  if not exist "%OUT%\%%T.exe" (
    echo %%T did not compile
    set FAILED=1
  ) else (
    echo.
    echo === %%T ===
    call "%OUT%\%%T.exe"
    if errorlevel 1 set FAILED=1
  )
)

popd
if "!FAILED!"=="1" (
  echo.
  echo A TEST FAILED
  exit /b 1
)
echo.
echo all five pass
endlocal
