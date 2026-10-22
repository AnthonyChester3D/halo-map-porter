#!/bin/sh
# build.sh: makes HaloMapPorter.exe with MinGW-w64.
#   Linux / WSL: the x86_64-w64-mingw32 cross compiler (Debian/Ubuntu: g++-mingw-w64-x86-64)
#   Windows: MSYS2's "MINGW64" shell, as  CXX=g++ CC=gcc WINDRES=windres ./build.sh
set -e
CXX=${CXX:-x86_64-w64-mingw32-g++}; CC=${CC:-x86_64-w64-mingw32-gcc}; WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
mkdir -p build
$WINDRES app.rc -O coff -o build/app.res
for f in core gui_win inflate mapcard mapcard_win maplist_edit maplist_win mappack contents_win lodview_win modelview_win perf portals levelshaders shaders_win glpreview_win preview render tagfile lod; do
  echo "CXX $f.cpp"
  $CXX -std=c++17 -O2 -municode -DUNICODE -D_UNICODE -c $f.cpp -o build/$f.o
done
for f in allocator simplifier stripifier; do   # meshoptimizer (MIT, see meshoptimizer/LICENSE.md)
  echo "CXX meshoptimizer/$f.cpp"
  $CXX -std=c++17 -O2 -c meshoptimizer/$f.cpp -o build/meshopt_$f.o
done
$CC -O2 -c stb_impl.c -o build/stb_impl.o
$CXX -municode -mwindows -static -O2 -s -o HaloMapPorter.exe build/*.o build/app.res \
  -lcomctl32 -lcomdlg32 -lgdi32 -lole32 -lopengl32 -lshell32 -lurlmon -luuid
echo "built HaloMapPorter.exe"
