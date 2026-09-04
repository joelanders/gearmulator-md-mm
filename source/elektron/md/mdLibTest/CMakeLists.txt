cmake_minimum_required(VERSION 3.15)

add_executable(mdLibTest mdRuntimeTest.cpp)
target_link_libraries(mdLibTest PRIVATE mdLib)

add_test(NAME mdLibTests COMMAND mdLibTest)
set_tests_properties(mdLibTests PROPERTIES LABELS "UnitTest")

set_property(TARGET mdLibTest PROPERTY FOLDER "Elektron")

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
add_test(NAME mdUwFirmwareTest COMMAND mdUwFirmwareTest)
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
