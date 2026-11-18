@echo off
rem Borderlands 4 Trainer - build script (Visual Studio 2022 or newer, x64).
rem Output: build\native.dll + build\BL4Trainer.exe
setlocal
set ROOT=%~dp0
set TP=%ROOT%third_party
set MH=%TP%\minhook
set IMG=%TP%\imgui-1.91.9b
set OUT=%ROOT%build
set OBJ=%ROOT%build\obj
if not exist "%OBJ%" mkdir "%OBJ%"
if "%VCINSTALLDIR%"=="" (
  for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSDIR=%%i
)
if "%VCINSTALLDIR%"=="" call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || (echo Visual Studio C++ tools not found & exit /b 1)
set INC=/I"%ROOT%src" /I"%TP%" /I"%IMG%" /I"%IMG%\backends" /I"%MH%\include"
set FLAGS=/nologo /O2 /MT /EHsc /std:c++17 /utf-8 /W3 /DNDEBUG /DNOMINMAX /DWB_PUBLIC=1 /DIMGUI_DISABLE_DEBUG_TOOLS /wd4996 /wd4244 /wd4267

if not exist "%OBJ%\hde64.obj" (
  echo === minhook
  cl %FLAGS% %INC% /c /Fo"%OBJ%\\" "%MH%\src\buffer.c" "%MH%\src\hook.c" "%MH%\src\trampoline.c" "%MH%\src\hde\hde64.c" || exit /b 1
)
if not exist "%OBJ%\imgui_impl_dx12.obj" (
  echo === imgui
  cl %FLAGS% %INC% /c /Fo"%OBJ%\\" "%IMG%\imgui.cpp" "%IMG%\imgui_draw.cpp" "%IMG%\imgui_tables.cpp" "%IMG%\imgui_widgets.cpp" "%IMG%\backends\imgui_impl_win32.cpp" "%IMG%\backends\imgui_impl_dx12.cpp" || exit /b 1
)
echo === trainer
cl %FLAGS% %INC% /c /Fo"%OBJ%\\" "%ROOT%src\core.cpp" "%ROOT%src\overlay.cpp" "%ROOT%src\panel.cpp" "%ROOT%src\settings.cpp" "%ROOT%src\dx12_hook.cpp" "%ROOT%src\skin.cpp" "%ROOT%src\ue.cpp" "%ROOT%src\bl4.cpp" "%ROOT%src\savefile.cpp" "%ROOT%src\serial.cpp" "%ROOT%src\items.cpp" "%ROOT%src\report.cpp" "%ROOT%src\trinity\hooks\xinput_hook.cpp" "%ROOT%src\trinity\hooks\dualsense.cpp" || exit /b 1
echo === version resources
rc /nologo /i "%ROOT%res" /fo "%OBJ%\dll_version.res" "%ROOT%res\dll.rc" || exit /b 1
rc /nologo /i "%ROOT%res" /fo "%OBJ%\loader_version.res" "%ROOT%res\loader.rc" || exit /b 1
echo === link
link /nologo /DLL /OUT:"%OUT%\native.dll" "%OBJ%\*.obj" "%OBJ%\dll_version.res" user32.lib gdi32.lib psapi.lib d3d12.lib dxgi.lib dwmapi.lib imm32.lib version.lib d3dcompiler.lib xinput9_1_0.lib hid.lib setupapi.lib ole32.lib windowscodecs.lib advapi32.lib || exit /b 1
echo === loader
cl /nologo /O2 /MT /EHsc /std:c++17 /DUNICODE /D_UNICODE /DWB_PUBLIC=1 "%ROOT%loader\loader.cpp" /Fo"%OUT%\loader.obj.tmp" /Fe"%OUT%\BL4Trainer.exe" /link "%OBJ%\loader_version.res" user32.lib shell32.lib || exit /b 1
del "%OUT%\loader.obj.tmp" 2>nul
echo BUILD OK
endlocal
