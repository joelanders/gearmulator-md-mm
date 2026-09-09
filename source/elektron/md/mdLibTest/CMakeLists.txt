cmake_minimum_required(VERSION 3.15)

add_executable(mdLibTest mdRuntimeTest.cpp)
target_link_libraries(mdLibTest PRIVATE mdLib)

add_executable(mmBootFirmwareTest mmBootFirmwareTest.cpp)
target_link_libraries(mmBootFirmwareTest PRIVATE mdLib)
add_test(NAME mmBootFirmwareTest COMMAND mmBootFirmwareTest)
set_tests_properties(mmBootFirmwareTest PROPERTIES
	LABELS "Integration;FirmwareTest" SKIP_RETURN_CODE 77 TIMEOUT 180)
set_property(TARGET mmBootFirmwareTest PROPERTY FOLDER "Elektron/test")
add_dependencies(mdLibTest mmBootFirmwareTest)

add_test(NAME mdLibTests COMMAND mdLibTest)
set_tests_properties(mdLibTests PROPERTIES LABELS "UnitTest")

set_property(TARGET mdLibTest PROPERTY FOLDER "Elektron")

add_executable(mdUartRegisterTest uartRegisterTest.cpp)
target_link_libraries(mdUartRegisterTest PRIVATE mdLib)
add_test(NAME mdUartRegisterTest COMMAND mdUartRegisterTest)
set_tests_properties(mdUartRegisterTest PROPERTIES LABELS "UnitTest;Uart" TIMEOUT 10)
set_property(TARGET mdUartRegisterTest PROPERTY FOLDER "Elektron/test")

# Compile Sim directly so the synthetic CPU uses its own memory callbacks.
add_executable(mdUartCpuInterruptTest uartCpuInterruptTest.cpp ../mdLib/mdsim.cpp)
target_link_libraries(mdUartCpuInterruptTest PRIVATE 68kEmu)
target_include_directories(mdUartCpuInterruptTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
add_test(NAME mdUartCpuInterruptTest COMMAND mdUartCpuInterruptTest)
set_tests_properties(mdUartCpuInterruptTest PROPERTIES LABELS "UnitTest;Uart" TIMEOUT 10)
set_property(TARGET mdUartCpuInterruptTest PROPERTY FOLDER "Elektron/test")

add_executable(mdTurboMidiUnitTest turboMidiTest.cpp)
find_package(Threads REQUIRED)
target_link_libraries(mdTurboMidiUnitTest PRIVATE mdLib Threads::Threads)
add_test(NAME mdTurboMidiUnitTest COMMAND mdTurboMidiUnitTest)
set_tests_properties(mdTurboMidiUnitTest PROPERTIES LABELS "UnitTest;Midi")
set_property(TARGET mdTurboMidiUnitTest PROPERTY FOLDER "Elektron")

# Manual acceptance test: user-supplied firmware/data paths make this unsuitable
# for ctest, but it exercises the same validated sender against real firmware and
# requires an observable machine-state mutation after the UART drains.
add_executable(mdUserSysexFirmwareTest userSysexFirmwareTest.cpp)
target_link_libraries(mdUserSysexFirmwareTest PRIVATE mdLib)
set_property(TARGET mdUserSysexFirmwareTest PROPERTY FOLDER "Elektron/test")

add_executable(mdMidiTimingTest midiTimingTest.cpp)
target_link_libraries(mdMidiTimingTest PRIVATE mdLib)
add_test(NAME mdMidiTimingTest COMMAND mdMidiTimingTest)
add_test(NAME mdMidiTimingFirmwareTest COMMAND mdMidiTimingTest --firmware)
set_tests_properties(mdMidiTimingTest PROPERTIES LABELS "UnitTest;Midi;Timing" FIXTURES_SETUP MdMidiTiming)
set_tests_properties(mdMidiTimingFirmwareTest PROPERTIES
	LABELS "Integration;FirmwareTest;Midi;Timing" SKIP_RETURN_CODE 77 TIMEOUT 90)
set_property(TARGET mdMidiTimingTest PROPERTY FOLDER "Elektron/test")
add_dependencies(mdLibTest mdMidiTimingTest)

add_executable(mdHostRxTimingTest hostRxTimingTest.cpp)
target_link_libraries(mdHostRxTimingTest PRIVATE mdLib)
add_test(NAME mdHostRxTimingTest COMMAND mdHostRxTimingTest)
set_tests_properties(mdHostRxTimingTest PROPERTIES LABELS "UnitTest")
set_property(TARGET mdHostRxTimingTest PROPERTY FOLDER "Elektron")

add_executable(mdHostRxFirmwareTest hostRxFirmwareTest.cpp)
target_link_libraries(mdHostRxFirmwareTest PRIVATE mdLib)
add_test(NAME mdHostRxFirmwareTest COMMAND mdHostRxFirmwareTest)
set_tests_properties(mdHostRxFirmwareTest PROPERTIES
	LABELS "Integration;FirmwareTest" SKIP_RETURN_CODE 77 TIMEOUT 60)
set_property(TARGET mdHostRxFirmwareTest PROPERTY FOLDER "Elektron/test")

add_executable(mdIdleSelfBranchTest idleSelfBranchTest.cpp)
target_link_libraries(mdIdleSelfBranchTest PRIVATE mdLib)
add_test(NAME mdIdleSelfBranchTest COMMAND mdIdleSelfBranchTest)
set_tests_properties(mdIdleSelfBranchTest PROPERTIES LABELS "UnitTest;Timing")
set_property(TARGET mdIdleSelfBranchTest PROPERTY FOLDER "Elektron/test")

add_executable(mdIdleSchedulerFirmwareTest idleSchedulerFirmwareTest.cpp)
target_link_libraries(mdIdleSchedulerFirmwareTest PRIVATE mdLib)
add_test(NAME mdIdleSchedulerFirmwareTest COMMAND mdIdleSchedulerFirmwareTest)
set_tests_properties(mdIdleSchedulerFirmwareTest PROPERTIES
	LABELS "Integration;FirmwareTest;Timing" SKIP_RETURN_CODE 77 TIMEOUT 120)
set_property(TARGET mdIdleSchedulerFirmwareTest PROPERTY FOLDER "Elektron/test")

if(BUILD_TESTING)
	# The existing CI gate builds mdLibTest and selects only mdLibTests.
	# Build its prerequisites and let CTest fixtures include them in that
	# focused run, so a core/receive regression cannot silently skip the gate.
	add_dependencies(mdLibTest mc68kColdFireTimingTest mc68kHdi08ReceiveTest
		mc68kColdFireDivideTest mdHostRxTimingTest mdHostRxFirmwareTest
		mdIdleSelfBranchTest mdIdleSchedulerFirmwareTest mdUartRegisterTest mdUartCpuInterruptTest)
	set_tests_properties(mdHostRxTimingTest PROPERTIES FIXTURES_SETUP MdTimedHostRx)
	set_tests_properties(mdIdleSelfBranchTest PROPERTIES FIXTURES_SETUP MdIdleSelfBranch)
	set_tests_properties(mdUartRegisterTest mdUartCpuInterruptTest PROPERTIES FIXTURES_SETUP MdUart)
	set_tests_properties(mdLibTests PROPERTIES FIXTURES_REQUIRED "Mc68kCore;MdTimedHostRx;MdIdleSelfBranch;MdUart;MdMidiTiming")
endif()

add_executable(mdFirmwareImageTest firmwareImageTest.cpp)
target_link_libraries(mdFirmwareImageTest PRIVATE mdLib)

add_test(NAME mdFirmwareImageTest COMMAND mdFirmwareImageTest)
set_tests_properties(mdFirmwareImageTest PROPERTIES LABELS "UnitTest")

set_property(TARGET mdFirmwareImageTest PROPERTY FOLDER "Elektron")

add_executable(mdAutomationMidiTest automationMidiTest.cpp)
target_link_libraries(mdAutomationMidiTest PRIVATE mdLib)
add_test(NAME mdAutomationMidiTest COMMAND mdAutomationMidiTest)
set_tests_properties(mdAutomationMidiTest PROPERTIES LABELS "UnitTest")
set_property(TARGET mdAutomationMidiTest PROPERTY FOLDER "Elektron")

add_executable(mdUwFirmwareTest mdUwFirmwareTest.cpp)
target_link_libraries(mdUwFirmwareTest PRIVATE mdLib)
# The audio oracle must detect NaN/Inf even in builds using -Ofast or /fp:fast.
if(MSVC)
	target_compile_options(mdUwFirmwareTest PRIVATE /fp:precise)
else()
	target_compile_options(mdUwFirmwareTest PRIVATE -fno-fast-math)
endif()
add_test(NAME mdUwFirmwareTest COMMAND mdUwFirmwareTest)
add_test(NAME mdRamAudioOracleTest COMMAND mdUwFirmwareTest --audio-oracle)
set_tests_properties(mdRamAudioOracleTest PROPERTIES LABELS "UnitTest;AudioIo")
set_tests_properties(mdUwFirmwareTest PROPERTIES
	LABELS "Integration;FirmwareTest;Uw"
	SKIP_RETURN_CODE 77
	TIMEOUT 900)
set_property(TARGET mdUwFirmwareTest PROPERTY FOLDER "Elektron")

add_executable(mdFlashTest mdFlashTest.cpp)
target_link_libraries(mdFlashTest PRIVATE mdLib)
add_test(NAME mdFlashTest COMMAND mdFlashTest)
set_tests_properties(mdFlashTest PROPERTIES LABELS "UnitTest")
set_property(TARGET mdFlashTest PROPERTY FOLDER "Elektron")

add_executable(mdStateTest mdStateTest.cpp)
target_link_libraries(mdStateTest PRIVATE mdLib)
add_test(NAME mdStateTest COMMAND mdStateTest)
set_tests_properties(mdStateTest PROPERTIES LABELS "UnitTest")
set_property(TARGET mdStateTest PROPERTY FOLDER "Elektron")

add_executable(mdAudioQueueTest mdAudioQueueTest.cpp)
target_link_libraries(mdAudioQueueTest PRIVATE mdLib)

add_test(NAME mdAudioQueueTest COMMAND mdAudioQueueTest)
set_tests_properties(mdAudioQueueTest PROPERTIES LABELS "UnitTest;AudioIo")

set_property(TARGET mdAudioQueueTest PROPERTY FOLDER "Elektron/test")

if(BUILD_TESTING)
	# Windows/macOS CI selects the audio gate rather than mdLibTests. Keep the
	# same UART prerequisites in both gates without changing workflow selectors.
	add_dependencies(mdAudioQueueTest mdUartRegisterTest mdUartCpuInterruptTest mdMidiTimingTest)
	set_tests_properties(mdAudioQueueTest PROPERTIES FIXTURES_REQUIRED "MdUart;MdMidiTiming")
endif()

add_executable(mdAudioFirmwareTest mdAudioFirmwareTest.cpp)
target_link_libraries(mdAudioFirmwareTest PRIVATE mdLib)

add_test(NAME mdAudioFirmwareTest COMMAND mdAudioFirmwareTest)
set_tests_properties(mdAudioFirmwareTest PROPERTIES
	LABELS "FirmwareTest;AudioIo"
	SKIP_RETURN_CODE 77)

set_property(TARGET mdAudioFirmwareTest PROPERTY FOLDER "Elektron/test")

# Shared external regressions for the independent MM workaround removals.
add_executable(mmAudioFirmwareTest mmAudioFirmwareTest.cpp)
target_link_libraries(mmAudioFirmwareTest PRIVATE mdLib)
if(MSVC)
	target_compile_options(mmAudioFirmwareTest PRIVATE /fp:precise)
else()
	target_compile_options(mmAudioFirmwareTest PRIVATE -fno-fast-math)
endif()
add_test(NAME mmAudioFirmwareTest COMMAND mmAudioFirmwareTest)
add_test(NAME mmSineFirmwareTest COMMAND mmAudioFirmwareTest --sine)
add_test(NAME mmSineMidiFirmwareTest COMMAND mmAudioFirmwareTest --sine-midi)
add_test(NAME mmInputFirmwareTest COMMAND mmAudioFirmwareTest --input)
add_test(NAME mmDigiproFirmwareTest COMMAND mmAudioFirmwareTest --digipro)
add_test(NAME mmDigiproEnsembleFirmwareTest COMMAND mmAudioFirmwareTest --digipro-ensemble)
add_test(NAME mmSineOracleTest COMMAND mmAudioFirmwareTest --sine-oracle)
set_tests_properties(mmAudioFirmwareTest mmSineFirmwareTest mmSineMidiFirmwareTest mmInputFirmwareTest PROPERTIES
	LABELS "Integration;FirmwareTest;AudioIo" SKIP_RETURN_CODE 77 TIMEOUT 180)
set_tests_properties(mmDigiproFirmwareTest mmDigiproEnsembleFirmwareTest PROPERTIES
	LABELS "Integration;FirmwareTest;AudioIo" SKIP_RETURN_CODE 77 TIMEOUT 600)
set_tests_properties(mmSineOracleTest PROPERTIES
	LABELS "UnitTest;AudioIo" FIXTURES_SETUP MmAudioOracle TIMEOUT 10)
set_property(TARGET mmAudioFirmwareTest PROPERTY FOLDER "Elektron/test")

if(BUILD_TESTING)
	# Existing core/audio CI selectors must execute the fixture-free oracle.
	add_dependencies(mdLibTest mmAudioFirmwareTest)
	add_dependencies(mdAudioQueueTest mmAudioFirmwareTest)
	set_property(TEST mdLibTests mdAudioQueueTest APPEND PROPERTY FIXTURES_REQUIRED MmAudioOracle)
endif()

# Manual packaged-plugin measurement fixture; owns only explicitly supplied data.
add_executable(mdLatencyFixture EXCLUDE_FROM_ALL
    ${CMAKE_SOURCE_DIR}/source/pluginTester/latency/create_transport_fixture.cpp)
target_link_libraries(mdLatencyFixture PRIVATE mdLib)
set_property(TARGET mdLatencyFixture PROPERTY FOLDER "Elektron/test")
