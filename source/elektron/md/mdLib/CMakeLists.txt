cmake_minimum_required(VERSION 3.10)

project(mdLib)

add_library(mdLib STATIC)

set(SOURCES
	mdautomation.cpp mdautomation.h
	mdautomationsync.cpp mdautomationsync.h
	mddevice.cpp mddevice.h
	mddsp.cpp mddsp.h
	mdflash.cpp mdflash.h
	mdfrontpanel.cpp mdfrontpanel.h
	mdhardware.cpp mdhardware.h
	mdhostclock.h
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
	mdtimedhostrx.h
)

target_sources(mdLib PRIVATE ${SOURCES})
source_group("source" FILES ${SOURCES})

target_link_libraries(mdLib PUBLIC hardwareLib dsp56kEmu)

set_property(TARGET mdLib PROPERTY FOLDER "Elektron")

target_include_directories(mdLib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Scheduling decisions must use the same binary64 operations on every host.
# Fused/reassociated frame arithmetic can cross an integer cycle boundary and
# change the order of emulated processors. Keep this restriction local to the
# scheduler translation unit; audio/JIT arithmetic keeps its existing options.
if(MSVC)
	set_property(SOURCE mdhardware.cpp APPEND PROPERTY COMPILE_OPTIONS /fp:strict)
else()
	set_property(SOURCE mdhardware.cpp APPEND PROPERTY COMPILE_OPTIONS
		-fno-fast-math -ffp-contract=off)
endif()
