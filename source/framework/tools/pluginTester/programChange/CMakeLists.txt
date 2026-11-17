set(vst3_sdk "${CMAKE_CURRENT_LIST_DIR}/../../../../3rdparty/JUCE/modules/juce_audio_processors/format_types/VST3_SDK")
set(vst3_host_sources
	${vst3_sdk}/pluginterfaces/base/funknown.cpp
	${vst3_sdk}/pluginterfaces/base/coreiids.cpp
	${vst3_sdk}/public.sdk/source/vst/vstinitiids.cpp
	${vst3_sdk}/public.sdk/source/vst/utility/stringconvert.cpp
	${vst3_sdk}/public.sdk/source/vst/hosting/hostclasses.cpp
	${vst3_sdk}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
	${vst3_sdk}/public.sdk/source/vst/hosting/module.cpp)
if(APPLE)
	list(APPEND vst3_host_sources ${vst3_sdk}/public.sdk/source/vst/hosting/module_mac.mm)
	set_source_files_properties(${vst3_sdk}/public.sdk/source/vst/hosting/module_mac.mm
		PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-x;objective-c++;-fobjc-arc")
elseif(WIN32)
	list(APPEND vst3_host_sources ${vst3_sdk}/public.sdk/source/vst/hosting/module_win32.cpp)
else()
	list(APPEND vst3_host_sources ${vst3_sdk}/public.sdk/source/vst/hosting/module_linux.cpp)
endif()
add_executable(vst3ProgramChangeTest vst3ProgramChangeTest.cpp ${vst3_host_sources})
target_include_directories(vst3ProgramChangeTest PRIVATE ${vst3_sdk})
target_compile_definitions(vst3ProgramChangeTest PRIVATE RELEASE=1)
if(APPLE)
	target_link_libraries(vst3ProgramChangeTest PRIVATE "-framework Cocoa")
elseif(UNIX)
	target_link_libraries(vst3ProgramChangeTest PRIVATE ${CMAKE_DL_LIBS} pthread)
endif()
set_property(TARGET vst3ProgramChangeTest PROPERTY FOLDER "Gearmulator/test")
