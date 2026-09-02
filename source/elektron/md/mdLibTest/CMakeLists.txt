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

add_executable(mdUwFirmwareTest mdUwFirmwareTest.cpp)
target_link_libraries(mdUwFirmwareTest PRIVATE mdLib)
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
