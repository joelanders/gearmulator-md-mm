cmake_minimum_required(VERSION 3.20)
project(dspAssertionValidation LANGUAGES C CXX)
include(CTest)

set(RUNTIME_SOURCE "" CACHE PATH "Pinned parent source")
set(DSP_SOURCE "" CACHE PATH "Pinned DSP source under test")
if(NOT EXISTS "${RUNTIME_SOURCE}/base.cmake" OR NOT EXISTS "${DSP_SOURCE}/source/CMakeLists.txt")
    message(FATAL_ERROR "RUNTIME_SOURCE and DSP_SOURCE must identify the source under test")
endif()
include("${RUNTIME_SOURCE}/base.cmake")
add_subdirectory("${DSP_SOURCE}/source" dsp)

add_executable(dspAssertContract assertContract.cpp)
target_include_directories(dspAssertContract PRIVATE "${DSP_SOURCE}/source")
add_test(NAME dspAssertContract COMMAND dspAssertContract)

add_executable(dspCorePerf corePerf.cpp)
target_link_libraries(dspCorePerf PRIVATE dsp56kEmu)

# Record the actual compiler and flags beside the test/benchmark receipts.
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/compiler-$<CONFIG>.txt" CONTENT
"compiler=${CMAKE_CXX_COMPILER}\nversion=${CMAKE_CXX_COMPILER_VERSION}\nconfig=$<CONFIG>\nflags=${CMAKE_CXX_FLAGS}\nrelease=${CMAKE_CXX_FLAGS_RELEASE}\ndebug=${CMAKE_CXX_FLAGS_DEBUG}\n")
