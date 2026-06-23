#!/bin/bash
set -e

echo "=== Build ==="
g++ -O2 -std=c++20 bwt.cpp -o bwt

echo ""
echo "=== Run (compress data/ -> output.enc, restore -> data_restored/) ==="
./bwt

echo ""
echo "=== Score ==="
ls -la output.enc
