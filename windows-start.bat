@echo off
REM Set the USDROOT environment variable to the relative path
set USDROOT=.\build\usd

REM Update the PATH variable to include USD binaries, libraries, and Python
set PATH=%USDROOT%\bin;%USDROOT%\lib;%USDROOT%\python;%USDROOT%\python\Scripts;%PATH%

REM Set the PYTHONPATH to include the USD Python libraries
set PYTHONPATH=%USDROOT%\lib\python;%PYTHONPATH%

REM Start usdtweak with any arguments passed to the batch file
.\build\RelWithDebInfo\usdtweak.exe %*
