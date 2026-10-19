# Offline unit tests for the apx pure-logic headers (no Barony build needed, seconds):
#   cmake -S tools/apx/units -B build/units && cmake --build build/units --config Release
#   build/units/Release/f4c_units_test.exe   (APX_F4C_UNITS_BREAK=1: the control, must fail)
#   build/units/Release/tier_units_test.exe  (D74 tier damage and band rank; APX_TIER_UNITS_BREAK=1: the control, must fail)
cmake_minimum_required(VERSION 3.16)
project(apx_units CXX)
set(CMAKE_CXX_STANDARD 17)
add_executable(f4c_units_test ../f4c_units_test.cpp)
target_include_directories(f4c_units_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../src)
add_executable(tier_units_test ../tier_units_test.cpp)
target_include_directories(tier_units_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../src)
# D75 physical poise (APX_POISE_UNITS_BREAK=1: the control, armour adds nothing, must fail)
add_executable(poise_units_test ../poise_units_test.cpp)
target_include_directories(poise_units_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../src)
