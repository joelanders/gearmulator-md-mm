cmake_minimum_required(VERSION 3.15)

add_executable(mdLibTest mdRuntimeTest.cpp)
target_link_libraries(mdLibTest PRIVATE mdLib)

add_test(NAME mdLibTests COMMAND mdLibTest)
set_tests_properties(mdLibTests PROPERTIES LABELS "UnitTest")

set_property(TARGET mdLibTest PROPERTY FOLDER "Elektron")

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

if(BUILD_TESTING)
	# The existing CI gate builds mdLibTest and selects only mdLibTests.
	# Build its prerequisites and let CTest fixtures include them in that
	# focused run, so a core/receive regression cannot silently skip the gate.
	add_dependencies(mdLibTest mc68kColdFireTimingTest mc68kHdi08ReceiveTest
		mc68kColdFireDivideTest mdHostRxTimingTest mdHostRxFirmwareTest)
	set_tests_properties(mdHostRxTimingTest PROPERTIES FIXTURES_SETUP MdTimedHostRx)
	set_tests_properties(mdLibTests PROPERTIES FIXTURES_REQUIRED "Mc68kCore;MdTimedHostRx")
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

add_executable(mdAudioFirmwareTest mdAudioFirmwareTest.cpp)
target_link_libraries(mdAudioFirmwareTest PRIVATE mdLib)

add_test(NAME mdAudioFirmwareTest COMMAND mdAudioFirmwareTest)
set_tests_properties(mdAudioFirmwareTest PROPERTIES
	LABELS "FirmwareTest;AudioIo"
	SKIP_RETURN_CODE 77)

set_property(TARGET mdAudioFirmwareTest PROPERTY FOLDER "Elektron/test")
