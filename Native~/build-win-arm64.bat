@echo off
REM Build basis_media_native (stub backend) for Windows ARM64.
REM
REM Uses Zig as the cross-compiler so this works without the MSVC
REM ARM64 toolset installed — `zig c++ -target aarch64-windows-gnu`
REM produces a valid ARM64 PE that Unity loads on Windows-on-ARM.
REM
REM Install zig first:  winget install zig.zig
REM
REM Output: Plugins/Windows/ARM64/basis_media_native.dll
setlocal
set HERE=%~dp0
set OUT=%HERE%..\Plugins\Windows\ARM64

where zig >nul 2>&1
if errorlevel 1 (
  echo zig not on PATH. Install with: winget install zig.zig
  exit /b 1
)

if not exist "%OUT%" mkdir "%OUT%"

zig c++ -target aarch64-windows-gnu -shared -O2 ^
  -I "%HERE%" -I "%HERE%unity" ^
  -o "%OUT%\basis_media_native.dll" ^
  "%HERE%basis_media_core.c" ^
  "%HERE%protocol\basis_url.c" ^
  "%HERE%protocol\basis_io.c" ^
  "%HERE%protocol\basis_http.c" ^
  "%HERE%protocol\basis_bitstream.c" ^
  "%HERE%protocol\basis_rtsp.c" ^
  "%HERE%protocol\basis_rtmp.c" ^
  "%HERE%protocol\basis_ts.c" ^
  "%HERE%protocol\basis_mp4.c" ^
  "%HERE%other\basis_decoder_stub.c" ^
  "%HERE%windows\basis_win_http.cpp" ^
  -lws2_32 -lwinhttp || goto :err

REM zig leaves an import .lib and a .pdb next to the DLL — Unity has no use
REM for them in the Plugins tree.
del /q "%OUT%\basis_media_core.lib" "%OUT%\basis_media_native.pdb" >nul 2>&1

echo.
echo Built: %OUT%\basis_media_native.dll
exit /b 0

:err
echo Build failed.
exit /b 1
