@echo off
REM Cross-build basis_media_native (stub backend) for Linux x86_64 FROM A
REM WINDOWS HOST using Zig. Zig ships glibc/musl headers so no Linux/WSL
REM sysroot is required.
REM
REM Install zig first:  winget install zig.zig
REM
REM Output: Plugins/Linux/x86_64/libbasis_media_native.so
setlocal
set HERE=%~dp0
set OUT=%HERE%..\Plugins\Linux\x86_64

where zig >nul 2>&1
if errorlevel 1 (
  echo zig not on PATH. Install with: winget install zig.zig
  exit /b 1
)

if not exist "%OUT%" mkdir "%OUT%"

zig cc -target x86_64-linux-gnu -shared -fPIC -O2 -fvisibility=hidden ^
  -I "%HERE%" -I "%HERE%unity" ^
  -o "%OUT%\libbasis_media_native.so" ^
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
  -lpthread -ldl || goto :err

echo.
echo Built: %OUT%\libbasis_media_native.so
exit /b 0

:err
echo Build failed.
exit /b 1
