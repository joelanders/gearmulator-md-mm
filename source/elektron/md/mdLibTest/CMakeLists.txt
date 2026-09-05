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

add_executable(mmBootFirmwareTest mmBootFirmwareTest.cpp)
target_link_libraries(mmBootFirmwareTest PRIVATE mdLib)
add_test(NAME mmBootFirmwareTest COMMAND mmBootFirmwareTest)
set_tests_properties(mmBootFirmwareTest PROPERTIES
	LABELS "Integration;FirmwareTest" SKIP_RETURN_CODE 77 TIMEOUT 180)
set_property(TARGET mmBootFirmwareTest PROPERTY FOLDER "Elektron/test")

add_executable(mmAudioFirmwareTest mmAudioFirmwareTest.cpp)
target_link_libraries(mmAudioFirmwareTest PRIVATE mdLib)
if(MSVC)
	target_compile_options(mmAudioFirmwareTest PRIVATE /fp:precise)
else()
	target_compile_options(mmAudioFirmwareTest PRIVATE -fno-fast-math)
endif()
add_test(NAME mmAudioFirmwareTest COMMAND mmAudioFirmwareTest)
add_test(NAME mmSineFirmwareTest COMMAND mmAudioFirmwareTest --sine)
add_test(NAME mmDigiproFirmwareTest COMMAND mmAudioFirmwareTest --digipro)
add_test(NAME mmDigiproEnsembleFirmwareTest COMMAND mmAudioFirmwareTest --digipro-ensemble)
add_test(NAME mmSineOracleTest COMMAND mmAudioFirmwareTest --sine-oracle)
set_tests_properties(mmSineOracleTest PROPERTIES LABELS "UnitTest;AudioIo")
set_tests_properties(mmAudioFirmwareTest mmSineFirmwareTest PROPERTIES
	LABELS "Integration;FirmwareTest;AudioIo" SKIP_RETURN_CODE 77 TIMEOUT 180)
set_tests_properties(mmDigiproFirmwareTest mmDigiproEnsembleFirmwareTest PROPERTIES
	LABELS "Integration;FirmwareTest;AudioIo" SKIP_RETURN_CODE 77 TIMEOUT 600)
set_property(TARGET mmAudioFirmwareTest PROPERTY FOLDER "Elektron/test")

add_executable(mdAudioFirmwareTest mdAudioFirmwareTest.cpp)
target_link_libraries(mdAudioFirmwareTest PRIVATE mdLib)

add_test(NAME mdAudioFirmwareTest COMMAND mdAudioFirmwareTest)
set_tests_properties(mdAudioFirmwareTest PROPERTIES
	LABELS "FirmwareTest;AudioIo"
	SKIP_RETURN_CODE 77)

set_property(TARGET mdAudioFirmwareTest PROPERTY FOLDER "Elektron/test")
