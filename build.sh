# g++, make and cmake must be installed on this machine
./bin/eo install genie clang spirv-tools spirv-cross \
 && ./bin/genie gmake \
 && make -j$(nproc) -C build config=release
