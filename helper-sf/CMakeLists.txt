cmake_minimum_required(VERSION 3.21)

# ReSaverHelper.dll for Starfield: one 64-bit SFSE plugin. It needs no SFSE sources, CommonLib or vcpkg packages:
# build with Visual Studio 2022 (x64). It reads its addresses from the Address Library for SFSE Plugins at run time.
project(ReSaverHelperSF VERSION 2.0.0 LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR "The Starfield helper is 64-bit: configure with -A x64 (cmake --preset vs2022-x64)")
endif()
if("${PROJECT_SOURCE_DIR}" STREQUAL "${PROJECT_BINARY_DIR}")
	message(FATAL_ERROR "in-source builds are not allowed")
endif()

# static C runtime (players need no Visual C++ redistributable)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Build output stays in the build folder only (build\Release). Nothing is copied anywhere else.

set(HELPER_SOURCES
	src/PCH.h src/SFSE.h src/Log.h src/Log.cpp src/Game.h src/Game.cpp src/Settings.h src/Settings.cpp
	src/Server.h src/Server.cpp src/Rescue.h src/Rescue.cpp src/Records.h src/Records.cpp src/Protocol.h src/main.cpp)
add_library(ReSaverHelper SHARED ${HELPER_SOURCES})
target_include_directories(ReSaverHelper PRIVATE src)
target_precompile_headers(ReSaverHelper PRIVATE src/PCH.h)
target_compile_features(ReSaverHelper PRIVATE cxx_std_20)
# /EHa: a fault inside a game call is caught by try/catch(...) and logged instead of taking the game down
target_compile_options(ReSaverHelper PRIVATE /EHa /MP /utf-8 /W4 /permissive- $<$<CONFIG:Release>:/Zi>)
target_compile_definitions(ReSaverHelper PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_link_options(ReSaverHelper PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
target_link_libraries(ReSaverHelper PRIVATE advapi32 shell32)
set_target_properties(ReSaverHelper PROPERTIES OUTPUT_NAME "ReSaverHelper")
