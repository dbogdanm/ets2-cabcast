@echo off
rem Builds out\dbm_phone.addon64 (needs Visual Studio 2022 / Build Tools with the C++ workload)
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d %~dp0
if not exist out mkdir out
set FL=/nologo /O2 /MT /EHsc /std:c++20 /bigobj /W3 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /Ithird_party\reshade\include /Ithird_party\imgui
cl %FL% /LD src\dbm_phone.cpp src\capture.cpp src\cdp.cpp /Fe:out\dbm_phone.addon64 /Foout\ /link /OPT:REF /OPT:ICF /DLL d3d11.lib dxgi.lib windowsapp.lib user32.lib dwmapi.lib shell32.lib ole32.lib shlwapi.lib winhttp.lib advapi32.lib
