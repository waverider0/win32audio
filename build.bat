@echo off
mkdir out
cd out

cl /std:c++17 /Od /Zi /FC ../demo_sine.cpp
cl /std:c++17 /Od /Zi /FC ../demo_wav.cpp

cd ..