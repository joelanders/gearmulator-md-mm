cmake_minimum_required(VERSION 3.10)
project(synthLib)

add_library(synthLib STATIC)

set(SOURCES
	midi/songReset.h midi/songReset.cpp
	midi/midiFile.h midi/smfReader.cpp midi/rcpDecoder.cpp midi/rcpInternal.h midi/rcpTrack.cpp midi/rcpTempo.cpp
	analog/analogFilter.cpp analog/analogFilter.h
	arpeggiator.cpp arpeggiator.h
	audiobuffer.cpp audiobuffer.h
	audioTypes.h
	boardThread.h
	consoleTools.cpp consoleTools.h
	dac.cpp dac.h
	dcFilter.h
	device.cpp device.h
	deviceException.cpp deviceException.h
	deviceTypes.h
	functionRef.h
	dspClockDevice.h
	frontPanelSysex.cpp frontPanelSysex.h
	hostAudioWorkgroup.cpp hostAudioWorkgroup.h
	lcdfonts.cpp lcdfonts.h
	lv2PresetExport.cpp lv2PresetExport.h
	midiBufferParser.cpp midiBufferParser.h
	midiClock.cpp midiClock.h
	midiControllerThinner.cpp midiControllerThinner.h
	midiInputQueue.h
	midiRateLimiter.cpp midiRateLimiter.h
	midiRoutingMatrix.cpp midiRoutingMatrix.h
	midiToSysex.cpp midiToSysex.h
	midiTranslator.cpp midiTranslator.h
	midiTypes.h
	os.cpp os.h
	plugin.cpp plugin.h
	realtimeInstrumentation.cpp realtimeInstrumentation.h
	performanceReport.cpp performanceReport.h
	mameResamplers.cpp mameResamplers.h
	resampler.cpp resampler.h
	resamplerInOut.cpp resamplerInOut.h
	sampleRateTime.h
	romLoader.cpp romLoader.h
	sounddiverLibLoader.cpp sounddiverLibLoader.h
	sysexRemoteControl.cpp sysexRemoteControl.h
	sysexToMidi.cpp sysexToMidi.h
	syntheticAudioTestDevice.h
	vstpreset.cpp vstpreset.h
	wavReader.cpp wavReader.h
	wavTypes.h
	wavWriter.cpp wavWriter.h
)

target_sources(synthLib PRIVATE ${SOURCES})
if(MSVC)
	set_source_files_properties(midi/rcpTempo.cpp PROPERTIES COMPILE_OPTIONS /fp:precise)
else()
	set_source_files_properties(midi/rcpTempo.cpp PROPERTIES COMPILE_OPTIONS -fno-finite-math-only)
endif()
# Beat-position subtraction and finite-value guards must retain IEEE semantics
# even when the audio/DSP targets use fast-math. plugin.cpp sanitizes the host's
# transport values (NaN position/tempo) before anything else reads them.
if(MSVC)
	set_property(SOURCE midiClock.cpp plugin.cpp APPEND PROPERTY COMPILE_OPTIONS /fp:precise)
else()
	set_property(SOURCE midiClock.cpp plugin.cpp APPEND PROPERTY COMPILE_OPTIONS -fno-fast-math)
endif()
source_group("source" FILES ${SOURCES})

if(EMSCRIPTEN)
	# Of dsp56kBase, synthLib uses headers and the logger. The rest of that library is threads
	# and OS glue that does not build for the web, so the one source file is compiled here.
	get_target_property(DSP56KBASE_DIR dsp56kBase SOURCE_DIR)
	target_sources(synthLib PRIVATE ${DSP56KBASE_DIR}/logging.cpp)
	target_link_libraries(synthLib PUBLIC resample baseLib)
	target_include_directories(synthLib PUBLIC $<TARGET_PROPERTY:dsp56kBase,INTERFACE_INCLUDE_DIRECTORIES>)
else()
	target_link_libraries(synthLib PUBLIC resample dsp56kBase baseLib)
endif()

if(NOT MSVC)
	target_link_libraries(synthLib PUBLIC dl)
endif()

if(APPLE)
	target_link_libraries(synthLib PRIVATE "-framework CoreAudio")	# hostAudioWorkgroup.cpp
endif()

if(UNIX AND NOT APPLE)
	set(THREADS_PREFER_PTHREAD_FLAG TRUE)
	find_package(Threads REQUIRED)
	target_link_libraries(synthLib PRIVATE Threads::Threads)
endif()

set_property(TARGET synthLib PROPERTY FOLDER "Gearmulator")

target_include_directories(synthLib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

if(BUILD_TESTING)
	add_executable(arpeggiator_test arpeggiator_test.cpp)
	target_link_libraries(arpeggiator_test PRIVATE synthLib)
	set_property(TARGET arpeggiator_test PROPERTY FOLDER "Tests")
	add_test(NAME arpeggiator COMMAND arpeggiator_test)

	add_executable(dcFilter_test dcFilter_test.cpp)
	target_link_libraries(dcFilter_test PRIVATE synthLib)
	set_property(TARGET dcFilter_test PROPERTY FOLDER "Tests")
	add_test(NAME dcFilter COMMAND dcFilter_test)

	add_executable(dynamicSamplerate_test dynamicSamplerate_test.cpp)
	target_link_libraries(dynamicSamplerate_test PRIVATE synthLib)
	set_property(TARGET dynamicSamplerate_test PROPERTY FOLDER "Tests")
	add_test(NAME dynamicSamplerate COMMAND dynamicSamplerate_test)

	add_executable(resamplerLatency_test resamplerLatency_test.cpp)
	target_link_libraries(resamplerLatency_test PRIVATE synthLib)
	set_property(TARGET resamplerLatency_test PROPERTY FOLDER "Tests")
	add_test(NAME resamplerLatency COMMAND resamplerLatency_test)

	add_executable(boardThread_test boardThread_test.cpp)
	target_link_libraries(boardThread_test PRIVATE synthLib)
	set_property(TARGET boardThread_test PROPERTY FOLDER "Tests")
	add_test(NAME boardThread COMMAND boardThread_test)

	add_executable(midiRateLimiter_test midiRateLimiter_test.cpp)
	target_link_libraries(midiRateLimiter_test PRIVATE synthLib)
	set_property(TARGET midiRateLimiter_test PROPERTY FOLDER "Tests")
	add_test(NAME midiRateLimiter COMMAND midiRateLimiter_test)

	add_executable(midiBufferParser_test midiBufferParser_test.cpp)
	target_link_libraries(midiBufferParser_test PRIVATE synthLib)
	set_property(TARGET midiBufferParser_test PROPERTY FOLDER "Tests")
	add_test(NAME midiBufferParser COMMAND midiBufferParser_test)

	add_executable(midiControllerThinner_test midiControllerThinner_test.cpp)
	target_link_libraries(midiControllerThinner_test PRIVATE synthLib)
	set_property(TARGET midiControllerThinner_test PROPERTY FOLDER "Tests")
	add_test(NAME midiControllerThinner COMMAND midiControllerThinner_test)

	add_executable(midiInputQueue_test midiInputQueue_test.cpp)
	target_link_libraries(midiInputQueue_test PRIVATE synthLib)
	set_property(TARGET midiInputQueue_test PROPERTY FOLDER "Tests")
	add_test(NAME midiInputQueue COMMAND midiInputQueue_test)
	set_tests_properties(midiInputQueue PROPERTIES TIMEOUT 30)

	add_executable(device_test device_test.cpp)
	target_link_libraries(device_test PRIVATE synthLib)
	set_property(TARGET device_test PROPERTY FOLDER "Tests")
	add_test(NAME device COMMAND device_test)
endif()

if(BUILD_TESTING)
	add_executable(stereoPeakLimiter_test stereoPeakLimiter_test.cpp)
	target_link_libraries(stereoPeakLimiter_test PRIVATE synthLib)
	add_test(NAME stereoPeakLimiter COMMAND stereoPeakLimiter_test)
	if(MSVC)
		target_compile_options(stereoPeakLimiter_test PRIVATE /fp:precise)
	else()
		target_compile_options(stereoPeakLimiter_test PRIVATE -fno-finite-math-only)
	endif()
endif()

if(BUILD_TESTING)
	add_executable(synthLibMidiClockTimingTest synthLibTest/midiClockTimingTest.cpp)
	target_link_libraries(synthLibMidiClockTimingTest PRIVATE synthLib)
	# The test feeds NaN tempo/position on purpose; under finite-math the NaN itself is undefined
	if(MSVC)
		target_compile_options(synthLibMidiClockTimingTest PRIVATE /fp:precise)
	else()
		target_compile_options(synthLibMidiClockTimingTest PRIVATE -fno-fast-math)
	endif()
	add_test(NAME synthLibMidiClockTimingTest COMMAND synthLibMidiClockTimingTest)
	set_tests_properties(synthLibMidiClockTimingTest PROPERTIES LABELS "UnitTest;Midi;Timing" TIMEOUT 30)

	add_executable(synthLibResamplerTimingTest synthLibTest/resamplerTimingTest.cpp)
	target_link_libraries(synthLibResamplerTimingTest PRIVATE synthLib)
	add_test(NAME synthLibResamplerTimingTest COMMAND synthLibResamplerTimingTest)
	set_tests_properties(synthLibResamplerTimingTest PROPERTIES LABELS "UnitTest;AudioIo;Midi;Timing")

	add_executable(synthLibStateTransactionTest synthLibTest/stateTransactionTest.cpp)
	target_link_libraries(synthLibStateTransactionTest PRIVATE synthLib)
	add_test(NAME synthLibStateTransactionTest COMMAND synthLibStateTransactionTest)
	set_tests_properties(synthLibStateTransactionTest PROPERTIES LABELS "UnitTest")
	set_property(TARGET synthLibStateTransactionTest PROPERTY FOLDER "Gearmulator")

	add_executable(midiRoutingMatrixTest synthLibTest/midiRoutingMatrixTest.cpp)
	target_link_libraries(midiRoutingMatrixTest PRIVATE synthLib)
	add_test(NAME midiRoutingMatrixTest COMMAND midiRoutingMatrixTest)
	set_tests_properties(midiRoutingMatrixTest PROPERTIES LABELS "UnitTest")
	set_property(TARGET midiRoutingMatrixTest PROPERTY FOLDER "Gearmulator/test")

	add_executable(synthLibAudioTest synthLibAudioTest.cpp)
	target_link_libraries(synthLibAudioTest PRIVATE synthLib)
	add_test(NAME synthLibAudioTest COMMAND synthLibAudioTest)
	set_tests_properties(synthLibAudioTest PROPERTIES LABELS "UnitTest;AudioIo")
	set_property(TARGET synthLibAudioTest PROPERTY FOLDER "Gearmulator/test")
	# Existing platform CI selects synthLibAudioTest. Keep the independent
	# timing oracles in that gate as well as in a full CTest run.
	add_dependencies(synthLibAudioTest synthLibMidiClockTimingTest synthLibResamplerTimingTest)
	set_tests_properties(synthLibMidiClockTimingTest synthLibResamplerTimingTest
		PROPERTIES FIXTURES_SETUP SynthTiming)
	set_tests_properties(synthLibAudioTest PROPERTIES FIXTURES_REQUIRED SynthTiming)

	add_executable(synthLibPerformanceReportTest synthLibTest/performanceReportTest.cpp)
	target_link_libraries(synthLibPerformanceReportTest PRIVATE synthLib)
	add_test(NAME synthLibPerformanceReportTest COMMAND synthLibPerformanceReportTest
		"${CMAKE_CURRENT_BINARY_DIR}/performance-report-test.jsonl")
	set_tests_properties(synthLibPerformanceReportTest PROPERTIES LABELS "UnitTest" TIMEOUT 30)
	add_test(NAME synthLibAudioInstrumentationTest COMMAND synthLibAudioTest)
	set_tests_properties(synthLibAudioInstrumentationTest PROPERTIES
		LABELS "UnitTest;AudioIo" ENVIRONMENT "GEARMULATOR_RT_INSTRUMENTATION=1" TIMEOUT 60)

	add_executable(synthLibRealtimeInstrumentationTest
		synthLibTest/realtimeInstrumentationTest.cpp)
	target_link_libraries(synthLibRealtimeInstrumentationTest PRIVATE synthLib)
	add_test(NAME synthLibRealtimeInstrumentationTest
		COMMAND synthLibRealtimeInstrumentationTest)
	set_tests_properties(synthLibRealtimeInstrumentationTest PROPERTIES
		LABELS "UnitTest")
	set_property(TARGET synthLibRealtimeInstrumentationTest PROPERTY
		FOLDER "Gearmulator/test")
endif()
