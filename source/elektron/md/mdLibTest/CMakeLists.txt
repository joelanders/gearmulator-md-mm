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

add_executable(mdAutomationMidiTest automationMidiTest.cpp)
target_link_libraries(mdAutomationMidiTest PRIVATE mdLib)
add_test(NAME mdAutomationMidiTest COMMAND mdAutomationMidiTest)
set_tests_properties(mdAutomationMidiTest PROPERTIES LABELS "UnitTest")
set_property(TARGET mdAutomationMidiTest PROPERTY FOLDER "Elektron")
