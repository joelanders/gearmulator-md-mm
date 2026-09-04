cmake_minimum_required(VERSION 3.10)
project(baseLib)

add_library(baseLib STATIC)

set(SOURCES
	binarystream.cpp binarystream.h
	commandline.cpp commandline.h
	compilerdefs.h
	configFile.cpp configFile.h
	endian.h
	event.cpp event.h
	filesystem.cpp filesystem.h
	hybridcontainer.h
	logging.cpp logging.h
	md5.cpp md5.h
	os.cpp os.h
	propertyMap.cpp propertyMap.h
	semaphore.h
)

target_sources(baseLib PRIVATE ${SOURCES})
source_group("source" FILES ${SOURCES})

if(NOT MSVC)
	target_link_libraries(baseLib PUBLIC dl)
endif()

set_property(TARGET baseLib PROPERTY FOLDER "Gearmulator")

target_include_directories(baseLib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

if(BUILD_TESTING)
	add_executable(baseLibBinaryStreamTest binarystreamTest.cpp)
	target_link_libraries(baseLibBinaryStreamTest PRIVATE baseLib)
	add_test(NAME baseLibBinaryStreamTest COMMAND baseLibBinaryStreamTest)
	set_tests_properties(baseLibBinaryStreamTest PROPERTIES LABELS "UnitTest")
	set_property(TARGET baseLibBinaryStreamTest PROPERTY FOLDER "Tests")
endif()
