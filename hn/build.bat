@echo off
rem Builds NeighborCraftMod.dll (and UE4SS, which it links against) with MSVC + Ninja.
rem Output: %USERPROFILE%\NeighborCraft-lab\build\hn\NeighborCraftMod\NeighborCraftMod.dll
setlocal
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=%USERPROFILE%\.cargo\bin;%PATH%
set BUILD=%USERPROFILE%\NeighborCraft-lab\build\hn
if not exist "%BUILD%\build.ninja" (
  cmake -S "%~dp0." -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Game__Shipping__Win64 || exit /b 1
)
cmake --build "%BUILD%" --target NeighborCraftMod || exit /b 1
endlocal
