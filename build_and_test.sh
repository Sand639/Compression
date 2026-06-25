#!/bin/bash
set -e

echo "=== Build ==="
g++ -O2 -std=c++20 \
    main.cpp transform.cpp huffman.cpp rangecoder.cpp cm.cpp \
    lzss.cpp filters.cpp pipeline.cpp io.cpp archive.cpp selftest.cpp \
    -o bwt

echo ""
echo "=== Run (compress data/ -> output.enc, restore -> data_restored/) ==="
./bwt

echo ""
echo "=== Score ==="
ls -la output.enc
