git submodule update --init --recursive

rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cd build
make