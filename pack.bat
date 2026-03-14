@echo off
setlocal

set ROOT=%~dp0
set OUT=%ROOT%windbgskill.zip
set TMP_DIR=%ROOT%_pack_tmp

set X86_SRC=%ROOT%windbgskill\Release
set X64_SRC=%ROOT%windbgskill\x64\Release

echo Packing windbgskill...

:: Clean up any previous temp dir
if exist "%TMP_DIR%" rmdir /s /q "%TMP_DIR%"
mkdir "%TMP_DIR%\bins\x86"
mkdir "%TMP_DIR%\bins\x64"

:: Copy DLL and PDB from VS output dirs
copy /y "%X86_SRC%\windbgskill.dll" "%TMP_DIR%\bins\x86\" >nul
copy /y "%X86_SRC%\windbgskill.pdb" "%TMP_DIR%\bins\x86\" >nul
copy /y "%X64_SRC%\windbgskill.dll" "%TMP_DIR%\bins\x64\windbgskill.dll" >nul
copy /y "%X64_SRC%\windbgskill.pdb" "%TMP_DIR%\bins\x64\windbgskill.pdb" >nul

:: Copy skills dir (xcopy preserves subdirectories)
xcopy /e /i /q "%ROOT%skills" "%TMP_DIR%\skills\" >nul

:: Delete old zip
if exist "%OUT%" del /f /q "%OUT%"

:: Create zip from temp dir contents
powershell -NoProfile -Command "Compress-Archive -Path '%TMP_DIR%\*' -DestinationPath '%OUT%'"

:: Clean up temp dir
rmdir /s /q "%TMP_DIR%"

if %errorlevel% neq 0 (
    echo ERROR: Failed to create zip.
    exit /b 1
)

echo Done: %OUT%
endlocal
