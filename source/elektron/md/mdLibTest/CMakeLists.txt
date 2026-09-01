cmake_minimum_required(VERSION 3.15)

add_executable(mdLibTest turboMidiTest.cpp)
target_link_libraries(mdLibTest PRIVATE mdLib)

add_test(NAME mdLibTests COMMAND mdLibTest)
set_tests_properties(mdLibTests PROPERTIES LABELS "UnitTest")

set_property(TARGET mdLibTest PROPERTY FOLDER "Elektron")

add_executable(mdFirmwareUpdateTest ../mdLib/test/mdfirmwareupdate_test.cpp)
target_link_libraries(mdFirmwareUpdateTest PRIVATE mdLib)
add_test(NAME mdFirmwareUpdateTest COMMAND mdFirmwareUpdateTest)
set_tests_properties(mdFirmwareUpdateTest PROPERTIES LABELS "UnitTest")
set_property(TARGET mdFirmwareUpdateTest PROPERTY FOLDER "Elektron")

add_executable(mdFlashTest ../mdLib/test/mdflash_test.cpp)
target_link_libraries(mdFlashTest PRIVATE mdLib)
add_test(NAME mdFlashTest COMMAND mdFlashTest)
set_tests_properties(mdFlashTest PROPERTIES LABELS "UnitTest")
set_property(TARGET mdFlashTest PROPERTY FOLDER "Elektron")

add_executable(mdPlusDriveTest ../mdLib/test/mdplusdrive_test.cpp)
target_link_libraries(mdPlusDriveTest PRIVATE mdLib)
add_test(NAME mdPlusDriveTest COMMAND mdPlusDriveTest)
set_tests_properties(mdPlusDriveTest PROPERTIES LABELS "UnitTest")
set_property(TARGET mdPlusDriveTest PROPERTY FOLDER "Elektron")

add_executable(mdStateTest ../mdLib/test/mdstate_test.cpp)
target_link_libraries(mdStateTest PRIVATE mdLib)
add_test(NAME mdStateTest COMMAND mdStateTest)
set_tests_properties(mdStateTest PROPERTIES LABELS "UnitTest")
set_property(TARGET mdStateTest PROPERTY FOLDER "Elektron")

# Manual firmware acceptance tests; intentionally not registered with ctest
# because they require user-supplied ROM and SysEx paths.
add_executable(mdTurboMidiTest ../mdLib/test/mdturbomidi_test.cpp)
target_link_libraries(mdTurboMidiTest PRIVATE mdLib)
set_property(TARGET mdTurboMidiTest PROPERTY FOLDER "Elektron")

add_executable(mmTurboMidiTest ../mdLib/test/mmturbomidi_test.cpp)
target_link_libraries(mmTurboMidiTest PRIVATE mdLib)
set_property(TARGET mmTurboMidiTest PROPERTY FOLDER "Elektron")

add_executable(mdAutomationMidiTest automationMidiTest.cpp)
target_link_libraries(mdAutomationMidiTest PRIVATE mdLib)
add_test(NAME mdAutomationMidiTest COMMAND mdAutomationMidiTest)
set_tests_properties(mdAutomationMidiTest PROPERTIES LABELS "UnitTest")
set_property(TARGET mdAutomationMidiTest PROPERTY FOLDER "Elektron")

add_executable(mdUwFirmwareTest mdUwFirmwareTest.cpp)
target_link_libraries(mdUwFirmwareTest PRIVATE mdLib)
set_property(TARGET mdUwFirmwareTest PROPERTY FOLDER "Elektron")
