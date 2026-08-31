cmake_minimum_required(VERSION 3.10)

project(mdLib)

add_library(mdLib STATIC)

set(SOURCES
	mdautomation.cpp mdautomation.h
	mddevice.cpp mddevice.h
	mddsp.cpp mddsp.h
	mdfrontpanel.cpp mdfrontpanel.h
	mdflash.cpp mdflash.h
	mdfirmwareupdate.cpp mdfirmwareupdate.h
	elektron-firmware-tool.LICENSE
	mdhardware.cpp mdhardware.h
	mdmc.cpp mdmc.h
	mdmemorymap.h
	mdmidiprotocol.h
	mdsysexautomation.cpp mdsysexautomation.h
	mdmmwaveforms.h
	mdrealtimemidiqueue.h
	mdpanel.cpp mdpanel.h
	mdsim.cpp mdsim.h
	mdrom.cpp mdrom.h
	mdromdata.cpp mdromdata.h
	mdromloader.cpp mdromloader.h
	mdstate.cpp mdstate.h
	mdsysextransfer.h
	mdturbomidi.cpp mdturbomidi.h
	mdtypes.h
)

target_sources(mdLib PRIVATE ${SOURCES})
source_group("source" FILES ${SOURCES})

target_link_libraries(mdLib PUBLIC hardwareLib dsp56kEmu)

option(GEARMULATOR_MD_BUILD_PROFILE_WORKLOAD
	"Build the asset-free MD/MM profile workload" OFF)
if(GEARMULATOR_MD_BUILD_PROFILE_WORKLOAD)
	add_executable(mdProfileWorkload profile/mdprofileworkload.cpp)
	target_link_libraries(mdProfileWorkload PRIVATE mdLib)
	set_property(TARGET mdProfileWorkload PROPERTY FOLDER "Elektron/profile")
	if(BUILD_TESTING)
		add_test(NAME mdProfileWorkloadTests
			COMMAND mdProfileWorkload 4410 both)
		add_test(NAME mdProfileWorkloadBoundedTests
			COMMAND mdProfileWorkload 4410 both)
		set_tests_properties(mdProfileWorkloadTests mdProfileWorkloadBoundedTests
			PROPERTIES LABELS "UnitTest")
		set_tests_properties(mdProfileWorkloadBoundedTests PROPERTIES
			ENVIRONMENT "GEARMULATOR_MDMM_BOUNDED_JIT=1")
	endif()
endif()
set_property(TARGET mdLib PROPERTY FOLDER "Elektron")

target_include_directories(mdLib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
