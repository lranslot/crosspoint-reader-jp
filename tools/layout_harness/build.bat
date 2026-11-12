@echo off
rem Builds layout_harness with MSVC. See README.md.
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set ROOT=%~dp0..\..
cl /nologo /std:c++20 /EHsc /utf-8 /O2 ^
  /I "%ROOT%\lib\Aozora" /I "%ROOT%\lib\Utf8" /I "%ROOT%\src\activities\reader" /I "%ROOT%\lib\Epub" "%ROOT%\lib\Epub\Epub\CjkBreaks.cpp" ^
  "%~dp0main.cpp" "%ROOT%\lib\Aozora\AozoraStrip.cpp" "%ROOT%\lib\Utf8\Utf8.cpp" ^
  /Fe:"%~dp0layout_harness.exe" /Fo"%~dp0obj\\"
endlocal
