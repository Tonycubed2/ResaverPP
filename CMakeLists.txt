cmake_minimum_required(VERSION 3.21)
project(ReSaverPP VERSION 2 LANGUAGES C CXX)

# ReSaver++ : C++ port of ReSaver (FallrimTools) by Mark Fairchild. Apache License 2.0.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (MSVC)
	add_compile_options(/utf-8 /MP)
	add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(RESAVERPP_GUI "Build the GUI" ON)
option(RESAVERPP_TESTS "Build tests" ON)

# ---- third party ------------------------------------------------------------------------
add_library(rs_thirdparty STATIC
	third_party/lz4/lz4.c
	third_party/miniz/miniz.c
	third_party/miniz/miniz_tdef.c
	third_party/miniz/miniz_tinfl.c)
target_include_directories(rs_thirdparty PUBLIC third_party/lz4 third_party/miniz)
target_compile_definitions(rs_thirdparty PUBLIC MINIZ_NO_ARCHIVE_APIS MINIZ_NO_STDIO MINIZ_NO_TIME)

# ---- core -------------------------------------------------------------------------------
file(GLOB RS_CORE_SOURCES CONFIGURE_DEPENDS src/core/*.cpp)
add_library(rs_core STATIC ${RS_CORE_SOURCES})
target_include_directories(rs_core PUBLIC src)
target_link_libraries(rs_core PUBLIC rs_thirdparty)

# ---- command line -----------------------------------------------------------------------
file(GLOB RS_CLI_SOURCES CONFIGURE_DEPENDS src/cli/*.cpp)
if (RS_CLI_SOURCES)
	add_executable(resaverpp-cli ${RS_CLI_SOURCES})
	target_link_libraries(resaverpp-cli PRIVATE rs_core)
	if (MINGW)
		target_link_options(resaverpp-cli PRIVATE -static)
	endif()
endif()

if (RESAVERPP_TESTS)
	add_executable(rs_roundtrip tests/roundtrip.cpp)
	target_link_libraries(rs_roundtrip PRIVATE rs_core)
	add_executable(rs_edit_test tests/edit_test.cpp)
	target_link_libraries(rs_edit_test PRIVATE rs_core)
	add_executable(rs_cf_test tests/cf_test.cpp)
	target_link_libraries(rs_cf_test PRIVATE rs_core)
	add_executable(rs_decomp_test tests/decomp_test.cpp)
	target_link_libraries(rs_decomp_test PRIVATE rs_core)
	add_executable(rs_psc_test tests/psc_test.cpp)
	target_link_libraries(rs_psc_test PRIVATE rs_core)
endif()

# ---- GUI --------------------------------------------------------------------------------
if (RESAVERPP_GUI AND EXISTS ${CMAKE_SOURCE_DIR}/src/gui/main.cpp)
	set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
	set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
	set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
	set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
	set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
	add_subdirectory(third_party/glfw EXCLUDE_FROM_ALL)
	find_package(OpenGL REQUIRED)

	add_library(rs_imgui STATIC
		third_party/imgui/imgui.cpp
		third_party/imgui/imgui_draw.cpp
		third_party/imgui/imgui_tables.cpp
		third_party/imgui/imgui_widgets.cpp
		third_party/imgui/imgui_demo.cpp
		third_party/imgui/backends/imgui_impl_glfw.cpp
		third_party/imgui/backends/imgui_impl_opengl3.cpp
		third_party/ImGuiColorTextEdit/TextEditor.cpp)
	target_include_directories(rs_imgui PUBLIC third_party/imgui third_party/imgui/backends third_party/ImGuiColorTextEdit)
	target_link_libraries(rs_imgui PUBLIC glfw OpenGL::GL)

	file(GLOB RS_GUI_SOURCES CONFIGURE_DEPENDS src/gui/*.cpp)
	add_executable(ReSaverPP WIN32 ${RS_GUI_SOURCES})
	target_link_libraries(ReSaverPP PRIVATE rs_core rs_imgui)
	if (WIN32)
		target_link_libraries(ReSaverPP PRIVATE comdlg32 shell32 ole32 advapi32)
		if (MSVC)
			target_link_options(ReSaverPP PRIVATE /ENTRY:wWinMainCRTStartup)
		else()
			target_link_options(ReSaverPP PRIVATE -municode -static)
		endif()
	endif()
endif()
