cmake_minimum_required(VERSION 3.15)

project(mdJucePlugin VERSION ${CMAKE_PROJECT_VERSION})

set(SOURCES
	mdController.cpp mdController.h
	mdEditor.cpp mdEditor.h
	mdFrontPanelPresentation.h
	mdMobilePanel.cpp mdMobilePanel.h
	mmMobilePanel.cpp mmMobilePanel.h
	mdPanelAffordances.h
	mdPluginEditorState.cpp mdPluginEditorState.h
	mdProductSkinPolicy.h
	mdProductSkins.h
	mdPluginProcessor.cpp mdPluginProcessor.h
	mdStorageImage.cpp mdStorageImage.h
	mdPluginEntry.cpp

	parameterDescriptions_md.json parameterDescriptions_mm.json

	skins/mdDefault/mdDefault.rml
	skins/mdDefault/mdKnobs.rcss
	skins/mdDefault/mdDefault.rcss
	skins/mdMobile/mdMobile.rml
	skins/mdMobile/mdMobile.rcss
	skins/mmSfx60/mmSfx60.rml
	skins/mmSfx60/mmKnobAtlas.svg
	skins/mmSfx60/mmKnobs.rcss
	skins/mmSfx60/mmSfx60.rcss
	skins/mmMobile/mmMobile.rml
	skins/mmMobile/mmMobile.rcss
	mdSettingsPanelFeel.cpp mdSettingsPanelFeel.h
)

set(COMMON_ASSETS
	"../../../jucePluginData/elektronKnobBlankAtlas.png"
	"../../../jucePluginData/elektronMasterKnobAtlas.png"
	"../../../jucePluginData/elektronButtonBevelAtlas.png"
	"../../../jucePluginData/elektronLedLens.png"
	"../../../jucePluginData/elektronLedLensSoft.png"
	"../../../jucePluginData/elektronPlayTriangle.png"
	"parameterDescriptions_md.json"
	"parameterDescriptions_mm.json"
	"../../../jucePluginData/elektron_panel.rcss"
	"../../../jucePluginData/elektron_materials.rcss")
file(GLOB MD_SKIN_ASSETS CONFIGURE_DEPENDS
	"skins/mdDefault/*.png" "skins/mdDefault/*.rml" "skins/mdDefault/*.rcss"
	"skins/mdDefault/*.ttf" "skins/mdDefault/*.lua"
	"skins/mdMobile/*.png" "skins/mdMobile/*.rml" "skins/mdMobile/*.rcss"
	"skins/mdMobile/*.ttf" "skins/mdMobile/*.lua")
file(GLOB MM_SKIN_ASSETS CONFIGURE_DEPENDS
	"skins/mmSfx60/*.png" "skins/mmSfx60/*.rml" "skins/mmSfx60/*.rcss"
	"skins/mmSfx60/*.ttf" "skins/mmSfx60/*.lua"
	"skins/mmMobile/*.png" "skins/mmMobile/*.rml" "skins/mmMobile/*.rcss"
	"skins/mmMobile/*.ttf" "skins/mmMobile/*.lua")

juce_add_binary_data(mdJucePlugin_BinaryData SOURCES
	${COMMON_ASSETS} "tus_settings_gui_Machinedrum.rml" ${MD_SKIN_ASSETS})
juce_add_binary_data(mmJucePlugin_BinaryData SOURCES
	${COMMON_ASSETS} "tus_settings_gui_Monomachine.rml" ${MM_SKIN_ASSETS})

createJucePlugin(mdJucePlugin "Gearmulator MD" TRUE "Tmdr" mdJucePlugin_BinaryData mdLib)
createJucePlugin(mmJucePlugin "Gearmulator MM" TRUE "Tmno" mmJucePlugin_BinaryData mdLib)

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
	set_target_properties(mdJucePlugin_Standalone mmJucePlugin_Standalone PROPERTIES
		XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED YES
		XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic)
endif()

# JUCE can populate bundle resources after its first ad-hoc signing pass on
# macOS. Re-sign the completed VST3 and Standalone bundles so hosts and Finder
# do not reject their final on-disk contents.
if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
	foreach(plugin_target
		mdJucePlugin_VST3 mmJucePlugin_VST3
		mdJucePlugin_AU mmJucePlugin_AU
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
	juce_add_plugin(mdAudioProbePlugin
		COMPANY_NAME "Gearmulator"
		IS_SYNTH TRUE
		NEEDS_MIDI_INPUT TRUE
		NEEDS_MIDI_OUTPUT FALSE
		IS_MIDI_EFFECT FALSE
		COPY_PLUGIN_AFTER_BUILD FALSE
		PLUGIN_MANUFACTURER_CODE GmPv
		PLUGIN_CODE Taip
		FORMATS VST3
		PRODUCT_NAME "Gearmulator Audio I-O Probe")
	target_sources(mdAudioProbePlugin PRIVATE mdAudioProbePlugin.cpp)
	removeJuceDependencies(mdAudioProbePlugin)
	target_link_libraries(mdAudioProbePlugin PRIVATE
		jucePluginLib juce_plugin_modules synthLib)
	target_compile_definitions(mdAudioProbePlugin PUBLIC
		JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1)

	add_executable(mdAudioIoLayoutTest mdAudioIoLayoutTest.cpp)
	target_link_libraries(mdAudioIoLayoutTest PRIVATE
		mdJucePlugin jucePluginEditorLib mdLib juce_plugin_modules
		juce::juce_opengl)
	target_compile_definitions(mdAudioIoLayoutTest PRIVATE
		JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1)
	set_property(TARGET mdAudioIoLayoutTest PROPERTY FOLDER "Elektron/test")
	add_test(NAME mdAudioIoLayoutTest COMMAND mdAudioIoLayoutTest)
	set_tests_properties(mdAudioIoLayoutTest PROPERTIES LABELS "UnitTest;AudioIo")

	foreach(plugin_target mdJucePlugin_VST3 mmJucePlugin_VST3)
		if(TARGET ${plugin_target} AND TARGET pluginTester)
			add_test(NAME ${plugin_target}AudioIoTest
				COMMAND $<TARGET_FILE:pluginTester> -verify-audio-buses
					-blocks 16 -plugin $<TARGET_FILE:${plugin_target}>)
			set_tests_properties(${plugin_target}AudioIoTest PROPERTIES
				LABELS "PluginTest;AudioIo")
		endif()
	endforeach()
	if(TARGET mdAudioProbePlugin_VST3 AND TARGET pluginTester)
		add_test(NAME mdAudioProbePluginVST3IdentityTest
			COMMAND $<TARGET_FILE:pluginTester> -verify-audio-identity
				-blocks 0 -samplerate 48000 -blocksize 64
				-plugin $<TARGET_FILE:mdAudioProbePlugin_VST3>)
		set_tests_properties(mdAudioProbePluginVST3IdentityTest PROPERTIES
			LABELS "PluginTest;AudioIo")
	endif()

	add_executable(mdFrontPanelPresentationTest mdFrontPanelPresentationTest.cpp)
	target_link_libraries(mdFrontPanelPresentationTest PRIVATE mdLib)
	add_test(NAME mdFrontPanelPresentationTests COMMAND mdFrontPanelPresentationTest)
	set_tests_properties(mdFrontPanelPresentationTests PROPERTIES LABELS "UnitTest")
	set_property(TARGET mdFrontPanelPresentationTest PROPERTY FOLDER "Elektron")

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
			mdJucePlugin jucePluginEditorLib mdLib juce_plugin_modules
			juce::juce_opengl)
		target_include_directories(${TARGET_NAME} PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}/../../..)
		target_compile_definitions(${TARGET_NAME} PRIVATE
			JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1)
		set_property(TARGET ${TARGET_NAME} PROPERTY FOLDER "Elektron/test")
	endfunction()

	add_md_automation_firmware_test(mdAutomationFirmwareTest
		mdAutomationFirmwareTest.cpp)
	add_test(NAME mdAutomationFirmwareTest COMMAND mdAutomationFirmwareTest)
	set_tests_properties(mdAutomationFirmwareTest PROPERTIES
		LABELS "Integration;FirmwareTest"
		SKIP_RETURN_CODE 77)

	add_md_automation_firmware_test(mdAutomationRobustnessTest
		mdAutomationRobustnessTest.cpp)
	add_test(NAME mdAutomationRobustnessTest COMMAND mdAutomationRobustnessTest)
	set_tests_properties(mdAutomationRobustnessTest PROPERTIES
		LABELS "Integration;FirmwareTest" SKIP_RETURN_CODE 77 TIMEOUT 300)
	add_test(NAME mdAutomationArchitectureTest COMMAND mdAutomationRobustnessTest
		--architecture-only)
	set_tests_properties(mdAutomationArchitectureTest PROPERTIES
		LABELS "Unit" TIMEOUT 60)

	add_md_automation_firmware_test(mdAutomationSoakTest
		mdAutomationSoakTest.cpp)
	add_test(NAME mdAutomationSoakTest COMMAND mdAutomationSoakTest
		--soak-writes=256 --multi-writes=128)
	set_tests_properties(mdAutomationSoakTest PROPERTIES
		LABELS "Integration;FirmwareTest" SKIP_RETURN_CODE 77 TIMEOUT 300)

	option(MD_AUTOMATION_REGISTER_FULL_SOAK_TEST
		"Register the full MD/MM automation soak with CTest" OFF)
	if(MD_AUTOMATION_REGISTER_FULL_SOAK_TEST)
		add_test(NAME mdAutomationFullSoakTest COMMAND mdAutomationSoakTest
			--soak-writes=8192 --multi-writes=2048)
		set_tests_properties(mdAutomationFullSoakTest PROPERTIES
			LABELS "Integration;FirmwareTest;Soak"
			SKIP_RETURN_CODE 77 TIMEOUT 900)
	endif()

	add_executable(mdProjectStateRestoreTest mdProjectStateRestoreTest.cpp)
	target_link_libraries(mdProjectStateRestoreTest PRIVATE
		mdJucePlugin jucePluginEditorLib mdLib juce_plugin_modules
		juce::juce_opengl)
	target_include_directories(mdProjectStateRestoreTest PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/../../..)
	target_compile_definitions(mdProjectStateRestoreTest PRIVATE
		JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1)
	add_test(NAME mdProjectStateRestoreTest COMMAND mdProjectStateRestoreTest)
	set_tests_properties(mdProjectStateRestoreTest PROPERTIES
		LABELS "Integration;FirmwareTest"
		SKIP_RETURN_CODE 77
		TIMEOUT 600)
	set_property(TARGET mdProjectStateRestoreTest PROPERTY FOLDER "Elektron/test")
endif()
