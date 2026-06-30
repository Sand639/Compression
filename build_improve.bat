@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -no_logo
cd /d D:\GitHub\Compression
cl /nologo /std:c++20 /O2 /EHsc /utf-8 improve_test.cpp transform.cpp huffman.cpp rangecoder.cpp cm.cpp lzss.cpp filters.cpp pipeline.cpp io.cpp archive.cpp selftest.cpp /Fe:improve_test.exe
echo BUILD_DONE_EXIT=%ERRORLEVEL%
