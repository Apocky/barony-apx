# F6 offline unit tests (tier T1, no Barony launch):
#   cmake -S tests/apx -B build-tests -G "Visual Studio 17 2022" -A x64
#   cmake --build build-tests --config Release
#   build-tests\Release\parts_test.exe data\apx tests\apx\itemtypes.txt
cmake_minimum_required(VERSION 3.16)
project(apx_tests CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  add_compile_options(/W3 /utf-8 /permissive- /O2)
endif()
set(APX_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../src/apx")
add_executable(parts_test parts_test.cpp "${APX_SRC}/parts_data.cpp" "${APX_SRC}/parts_roll.cpp")
