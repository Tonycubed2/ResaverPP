cmake_minimum_required(VERSION 3.21)

# ReSaverHelper.dll for Skyrim Legendary Edition (TESV.exe 1.9.32.0, SKSE 1.7.3): 32-bit.
# Build with Visual Studio 2022 for Win32 (see CMakePresets.json); nothing from vcpkg is needed.
project(ReSaverHelperLE VERSION 11.0.0 LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
	message(FATAL_ERROR "The Legendary Edition helper is 32-bit: configure with -A Win32 (cmake --preset vs2022-win32)")
endif()
if("${PROJECT_SOURCE_DIR}" STREQUAL "${PROJECT_BINARY_DIR}")
	message(FATAL_ERROR "in-source builds are not allowed")
endif()

# static C runtime (players need no Visual C++ redistributable), the same as SKSE's own projects
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Build output stays in the build folder only (build\Release). Nothing is copied anywhere else.

# ---- SKSE 1.7.3 (himika's libSKSE copy, extern\skse): a static library of only the files the helper uses.
# (Some of libSKSE's other files do not build any more, e.g. Utilities.cpp and the Scaleform ones; the
# helper needs none of them. The list is what the helper's code links to, followed through SKSE.)
set(SKSE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/skse")
set(SKSE_COMMON_SOURCES IDataStream IDebugLog IErrors IFileStream)
list(TRANSFORM SKSE_COMMON_SOURCES PREPEND "${SKSE_DIR}/common/")
list(TRANSFORM SKSE_COMMON_SOURCES APPEND ".cpp")
set(SKSE_SOURCES
	GameAPI GameBSExtraData GameData GameExtraData GameFormComponents GameForms GameMenus GameObjects GameRTTI
	GameReferences GameTypes HashUtil NiObjects PapyrusArgs PapyrusVM ScaleformAPI)
list(TRANSFORM SKSE_SOURCES PREPEND "${SKSE_DIR}/skse/")
list(TRANSFORM SKSE_SOURCES APPEND ".cpp")

add_library(skse_le STATIC ${SKSE_COMMON_SOURCES} ${SKSE_SOURCES})
target_include_directories(skse_le PUBLIC "${SKSE_DIR}" "${SKSE_DIR}/skse")
target_compile_definitions(skse_le PUBLIC WIN32 _WINDOWS RUNTIME RUNTIME_VERSION=0x09200000 _CRT_SECURE_NO_WARNINGS
	_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS _SILENCE_ALL_CXX20_DEPRECATION_WARNINGS)
# SKSE's code is written for the older, more lenient MSVC rules
target_compile_options(skse_le PRIVATE /std:c++17 /permissive /Zc:twoPhase- /EHsc /MP /W0 /FIcommon/IPrefix.h)

# ---- the helper
set(HELPER_SOURCES
	src/PCH.h src/Log.h src/Log.cpp src/LE.h src/LE.cpp src/Papyrus.h src/Papyrus.cpp src/Game.h src/Game.cpp
	src/Settings.h src/Settings.cpp src/Server.h src/Server.cpp src/Transplant.h src/Transplant.cpp
	src/Protocol.h src/main.cpp)
add_library(ReSaverHelper SHARED ${HELPER_SOURCES})
target_include_directories(ReSaverHelper PRIVATE src)
target_precompile_headers(ReSaverHelper PRIVATE src/PCH.h)
# C++20 for std::format; SKSE's headers need the lenient rules here too.
# /EHa: a read of game memory that fails (an access violation) is caught by try/catch(...) and logged.
target_compile_options(ReSaverHelper PRIVATE /std:c++20 /permissive /Zc:twoPhase- /EHa /MP /utf-8 /W3 /wd4244 /wd4267 /wd4996 /wd4305
	$<$<CONFIG:Release>:/Zi>)
target_link_options(ReSaverHelper PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
target_link_libraries(ReSaverHelper PRIVATE skse_le advapi32 shell32)
set_target_properties(ReSaverHelper PROPERTIES OUTPUT_NAME "ReSaverHelper")
