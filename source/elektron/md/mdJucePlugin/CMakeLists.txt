cmake_minimum_required(VERSION 3.15)

project(mdJucePlugin VERSION ${CMAKE_PROJECT_VERSION})

set(SOURCES
	mdController.cpp mdController.h
	mdEditor.cpp mdEditor.h
	mdFrontPanelPresentation.h
	mdPanelAffordances.h
	mdPluginEditorState.cpp mdPluginEditorState.h
	mdProductSkinPolicy.h
	mdProductSkins.h
	mdPluginProcessor.cpp mdPluginProcessor.h
	mdStandalonePlusDrivePersistence.cpp mdStandalonePlusDrivePersistence.h
	mdStorageImage.cpp mdStorageImage.h
	mdPluginEntry.cpp

	parameterDescriptions_md.json parameterDescriptions_mm.json

	skins/mdDefault/mdDefault.rml
	skins/mdDefault/mdKnobs.rcss
	skins/mdDefault/mdDefault.rcss
	skins/mmSfx60/mmSfx60.rml
	skins/mmSfx60/mmKnobAtlas.svg
	skins/mmSfx60/mmKnobs.rcss
	skins/mmSfx60/mmSfx60.rcss
	mdSettingsPanelFeel.cpp mdSettingsPanelFeel.h
)

set(COMMON_ASSETS
	"../../../framework/juce/jucePluginData/elektronKnobBlankAtlas.png"
	"../../../framework/juce/jucePluginData/elektronMasterKnobAtlas.png"
	"../../../framework/juce/jucePluginData/elektronButtonBevelAtlas.png"
	"../../../framework/juce/jucePluginData/elektronLedLens.png"
	"../../../framework/juce/jucePluginData/elektronLedLensSoft.png"
	"../../../framework/juce/jucePluginData/elektronPlayTriangle.png"
	"parameterDescriptions_md.json"
	"parameterDescriptions_mm.json"
	"../../../framework/juce/jucePluginData/elektron_panel.rcss"
	"../../../framework/juce/jucePluginData/elektron_materials.rcss")
file(GLOB MD_SKIN_ASSETS CONFIGURE_DEPENDS
	"skins/mdDefault/*.png" "skins/mdDefault/*.rml" "skins/mdDefault/*.rcss"
	"skins/mdDefault/*.ttf" "skins/mdDefault/*.lua")
file(GLOB MM_SKIN_ASSETS CONFIGURE_DEPENDS
	"skins/mmSfx60/*.png" "skins/mmSfx60/*.rml" "skins/mmSfx60/*.rcss"
	"skins/mmSfx60/*.ttf" "skins/mmSfx60/*.lua")

juce_add_binary_data(mdJucePlugin_BinaryData SOURCES
	${COMMON_ASSETS} "tus_settings_gui_Machinedrum.rml" ${MD_SKIN_ASSETS})
juce_add_binary_data(mmJucePlugin_BinaryData SOURCES
	${COMMON_ASSETS} "tus_settings_gui_Monomachine.rml" ${MM_SKIN_ASSETS})

createJucePlugin(mdJucePlugin "Gearmulator MD" TRUE "Tmdr" mdJucePlugin_BinaryData mdLib)
createJucePlugin(mmJucePlugin "Gearmulator MM" TRUE "Tmno" mmJucePlugin_BinaryData mdLib)

# JUCE can populate bundle resources after its first ad-hoc signing pass on
# macOS. Re-sign the completed VST3 and Standalone bundles so hosts and Finder
# do not reject their final on-disk contents.
if(APPLE)
	foreach(plugin_target
		mdJucePlugin_VST3 mmJucePlugin_VST3
		mdJucePlugin_Standalone mmJucePlugin_Standalone)
		if(TARGET ${plugin_target})
			add_custom_command(TARGET ${plugin_target} POST_BUILD
				COMMAND /usr/bin/codesign --force --deep --sign -
					"$<TARGET_BUNDLE_DIR:${plugin_target}>"
				COMMENT "Ad-hoc signing completed ${plugin_target} bundle"
				VERBATIM
			)
		endif()
	endforeach()
endif()

# Build an independent MM product from the shared editor/controller code.
target_compile_definitions(mmJucePlugin PRIVATE MD_JUCEPLUGIN_MONOMACHINE=1)

if(BUILD_TESTING)
	add_executable(mdAudioIoLayoutTest mdAudioIoLayoutTest.cpp)
	target_link_libraries(mdAudioIoLayoutTest PRIVATE
		mdJucePlugin jucePluginEditorLib mdLib juce_plugin_modules
		juce::juce_opengl)
	target_compile_definitions(mdAudioIoLayoutTest PRIVATE
		JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1)
	set_property(TARGET mdAudioIoLayoutTest PROPERTY FOLDER "Elektron/test")
	add_test(NAME mdAudioIoLayoutTest COMMAND mdAudioIoLayoutTest)
	set_tests_properties(mdAudioIoLayoutTest PROPERTIES LABELS "UnitTest")

	add_executable(mdShiftTriggerLatchTest mdShiftTriggerLatchTest.cpp)
	target_link_libraries(mdShiftTriggerLatchTest PRIVATE mdLib)
	target_include_directories(mdShiftTriggerLatchTest PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR})
	set_property(TARGET mdShiftTriggerLatchTest PROPERTY FOLDER "Elektron/test")
	add_test(NAME mdShiftTriggerLatchTest COMMAND mdShiftTriggerLatchTest)

	add_executable(mdAutomationParameterTest mdAutomationParameterTest.cpp)
	target_link_libraries(mdAutomationParameterTest PRIVATE
		jucePluginLib mdLib juce_plugin_modules)
	target_compile_definitions(mdAutomationParameterTest PRIVATE
		JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
		MD_AUTOMATION_PARAMETER_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
	set_property(TARGET mdAutomationParameterTest PROPERTY FOLDER "Elektron/test")
	add_test(NAME mdAutomationParameterTest COMMAND mdAutomationParameterTest)

	function(add_md_automation_firmware_test TARGET_NAME SOURCE_FILE)
		add_executable(${TARGET_NAME} ${SOURCE_FILE} mdAutomationTestSupport.h)
		target_link_libraries(${TARGET_NAME} PRIVATE
			mdJucePlugin jucePluginEditorLib mdLib juce_plugin_modules)
		target_include_directories(${TARGET_NAME} PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}/../../..)
		target_compile_definitions(${TARGET_NAME} PRIVATE
			JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1)
		set_property(TARGET ${TARGET_NAME} PROPERTY FOLDER "Elektron/test")
	endfunction()

	add_md_automation_firmware_test(mdAutomationFirmwareTest
		mdAutomationFirmwareTest.cpp)
	add_test(NAME mdAutomationFirmwareTest COMMAND mdAutomationFirmwareTest)
	set_tests_properties(mdAutomationFirmwareTest PROPERTIES LABELS "Integration")

	add_md_automation_firmware_test(mdAutomationRobustnessTest
		mdAutomationRobustnessTest.cpp)
	add_test(NAME mdAutomationRobustnessTest COMMAND mdAutomationRobustnessTest)
	set_tests_properties(mdAutomationRobustnessTest PROPERTIES
		LABELS "Integration" TIMEOUT 300)
	add_test(NAME mdAutomationArchitectureTest COMMAND mdAutomationRobustnessTest
		--architecture-only)
	set_tests_properties(mdAutomationArchitectureTest PROPERTIES
		LABELS "Unit" TIMEOUT 60)

	add_md_automation_firmware_test(mdAutomationSoakTest
		mdAutomationSoakTest.cpp)
	add_test(NAME mdAutomationSoakTest COMMAND mdAutomationSoakTest
		--soak-writes=256 --multi-writes=128)
	set_tests_properties(mdAutomationSoakTest PROPERTIES
		LABELS "Integration" TIMEOUT 300)

	add_executable(mdStandalonePlusDrivePersistenceTest
		mdStandalonePlusDrivePersistenceTest.cpp)
	target_link_libraries(mdStandalonePlusDrivePersistenceTest PRIVATE
		mdJucePlugin jucePluginEditorLib mdLib juce_plugin_modules)
	target_include_directories(mdStandalonePlusDrivePersistenceTest PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/../../..)
	target_compile_definitions(mdStandalonePlusDrivePersistenceTest PRIVATE
		JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1)
	set_property(TARGET mdStandalonePlusDrivePersistenceTest PROPERTY FOLDER "Elektron/test")
	add_test(NAME mdStandalonePlusDrivePersistenceTest
		COMMAND mdStandalonePlusDrivePersistenceTest)
	set_tests_properties(mdStandalonePlusDrivePersistenceTest PROPERTIES TIMEOUT 60)

	option(MD_AUTOMATION_REGISTER_FULL_SOAK_TEST
		"Register the full MD/MM automation soak with CTest" OFF)
	if(MD_AUTOMATION_REGISTER_FULL_SOAK_TEST)
		add_test(NAME mdAutomationFullSoakTest COMMAND mdAutomationSoakTest
			--soak-writes=8192 --multi-writes=2048)
		set_tests_properties(mdAutomationFullSoakTest PROPERTIES
			LABELS "Integration;Soak" TIMEOUT 900)
	endif()
endif()
