cmake_minimum_required(VERSION 3.16)

if(EXISTS "${CMAKE_SOURCE_DIR}/components")
	set(EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/components")
endif()
	set(PROJECT_VER "0.43.00")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ouispy-c6)
