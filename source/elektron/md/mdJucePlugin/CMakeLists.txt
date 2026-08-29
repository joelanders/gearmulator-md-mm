cmake_minimum_required(VERSION 3.15)

project(mdJucePlugin VERSION ${CMAKE_PROJECT_VERSION})

set(SOURCES
	mdController.cpp mdController.h
	mdEditor.cpp mdEditor.h
	mdPanelAffordances.h
	mdPluginEditorState.cpp mdPluginEditorState.h
	mdProductSkinPolicy.h
	mdProductSkins.h
	mdPluginProcessor.cpp mdPluginProcessor.h
	mdStorageImage.cpp mdStorageImage.h
	mdPluginEntry.cpp

	parameterDescriptions_md.json

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
	"../../../framework/juce/jucePluginData/elektronPlayTriangle.png"
	"parameterDescriptions_md.json"
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
