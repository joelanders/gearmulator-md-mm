cmake_minimum_required(VERSION 3.10)

project(mdLib)

add_library(mdLib STATIC)

set(SOURCES
	mddevice.cpp mddevice.h
	mddsp.cpp mddsp.h
	mdfrontpanel.cpp mdfrontpanel.h
	mdhardware.cpp mdhardware.h
	mdmc.cpp mdmc.h
	mdmemorymap.h
	mdmidiprotocol.h
	mdmmwaveforms.h
	mdrealtimemidiqueue.h
	mdpanel.cpp mdpanel.h
	mdsim.cpp mdsim.h
	mdrom.cpp mdrom.h
	mdromdata.cpp mdromdata.h
	mdromloader.cpp mdromloader.h
	mdstate.cpp mdstate.h
	mdtypes.h
)

target_sources(mdLib PRIVATE ${SOURCES})
source_group("source" FILES ${SOURCES})

target_link_libraries(mdLib PUBLIC hardwareLib dsp56kEmu)

set_property(TARGET mdLib PROPERTY FOLDER "Elektron")

target_include_directories(mdLib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
