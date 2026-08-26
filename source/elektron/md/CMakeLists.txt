cmake_minimum_required(VERSION 3.15)

add_subdirectory(mdLib)

if(BUILD_TESTING)
	add_subdirectory(mdLibTest)
endif()

if(${CMAKE_PROJECT_NAME}_BUILD_JUCEPLUGIN)
	add_subdirectory(mdJucePlugin)
endif()
