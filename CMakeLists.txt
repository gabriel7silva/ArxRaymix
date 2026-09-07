cmake_minimum_required(VERSION 3.12...3.29)
project(ArxRaymix)

# Change CMAKE_LIBRARY_ARCHITECTURE to ensure the right libs are used
if(CMAKE_CL_64)
	set(CMAKE_LIBRARY_ARCHITECTURE "x64")
else()
	set(CMAKE_LIBRARY_ARCHITECTURE "x86")
endif()

set(libs "${CMAKE_CURRENT_LIST_DIR}/libs")
set(CMAKE_PREFIX_PATH
	"${libs}/boost"
	"${libs}/freetype"
	"${libs}/glm"
	"${libs}/libepoxy"
	"${libs}/openal"
	"${libs}/sdl"
	"${libs}/zlib"
	"${libs}/dxc"
)

if(DEFINED CMAKE_FIND_ROOT_PATH)
	# Need to be able to find these using absolute paths
	if(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY STREQUAL NEVER)
		set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY LAST)
	endif()
	if(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE STREQUAL NEVER)
		set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE LAST)
	endif()
endif()

set(DATA_FILES "${CMAKE_CURRENT_LIST_DIR}/data")

if(NOT DEFINED BUILD_TESTS)
	# TODO Add CppUnit library
	set(BUILD_TESTS OFF)
endif()

add_subdirectory("arx")
# Runtime DLL POST_BUILD lives next to add_executable_shared(arx) in arx/CMakeLists.txt
# (CMake only allows TARGET POST_BUILD in the directory that created the target).
