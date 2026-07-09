@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -no_logo
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++20 /O2 /EHsc /utf-8 /Fe:measure.exe measure.cpp transform.cpp huffman.cpp rangecoder.cpp cm.cpp lzss.cpp filters.cpp pipeline.cpp io.cpp archive.cpp selftest.cpp
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++20 /O2 /EHsc /utf-8 /Fe:bwt.exe main.cpp transform.cpp huffman.cpp rangecoder.cpp cm.cpp lzss.cpp filters.cpp pipeline.cpp io.cpp archive.cpp selftest.cpp
