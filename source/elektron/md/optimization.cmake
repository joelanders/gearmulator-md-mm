# Apply optimization to the measured emulation libraries after their targets
# exist. Defaults preserve the ordinary build on every platform.
option(GEARMULATOR_MDMM_APPLE_THINLTO
	"Enable Apple Clang ThinLTO for MD/MM core Release builds" OFF)
option(GEARMULATOR_MDMM_APPLE_OPTIMIZE_DSP
	"Include DSP libraries in the selected MD/MM Apple optimizations" OFF)
set(GEARMULATOR_MDMM_APPLE_PGO_MODE "none" CACHE STRING
	"MD/MM Apple profile-guided optimization: none, generate, or use")
set_property(CACHE GEARMULATOR_MDMM_APPLE_PGO_MODE PROPERTY STRINGS none generate use)
set(GEARMULATOR_MDMM_APPLE_PGO_PROFILE "" CACHE FILEPATH
	"Merged profile from the same source, compiler and architecture")

if(NOT GEARMULATOR_MDMM_APPLE_PGO_MODE MATCHES "^(none|generate|use)$")
	message(FATAL_ERROR "GEARMULATOR_MDMM_APPLE_PGO_MODE must be none, generate, or use")
endif()

if(NOT GEARMULATOR_MDMM_APPLE_THINLTO
	AND GEARMULATOR_MDMM_APPLE_PGO_MODE STREQUAL "none")
	return()
endif()

if(NOT APPLE OR NOT CMAKE_C_COMPILER_ID MATCHES "^(AppleClang|Clang)$"
	OR NOT CMAKE_CXX_COMPILER_ID MATCHES "^(AppleClang|Clang)$")
	message(FATAL_ERROR "MD/MM Apple optimization requires Clang on macOS")
endif()

if(NOT GEARMULATOR_MDMM_APPLE_PGO_MODE STREQUAL "none")
	if(NOT GEARMULATOR_MDMM_APPLE_THINLTO)
		message(FATAL_ERROR "Enable GEARMULATOR_MDMM_APPLE_THINLTO for MD/MM PGO")
	endif()
	list(LENGTH CMAKE_OSX_ARCHITECTURES _mdmm_arch_count)
	if(NOT _mdmm_arch_count EQUAL 1)
		message(FATAL_ERROR "MD/MM PGO requires one explicit CMAKE_OSX_ARCHITECTURES value; train and build each architecture separately")
	endif()
endif()

set(_mdmm_pgo_option "")
if(GEARMULATOR_MDMM_APPLE_PGO_MODE STREQUAL "generate")
	set(_mdmm_pgo_option "-fprofile-instr-generate")
elseif(GEARMULATOR_MDMM_APPLE_PGO_MODE STREQUAL "use")
	if(NOT EXISTS "${GEARMULATOR_MDMM_APPLE_PGO_PROFILE}"
		OR IS_DIRECTORY "${GEARMULATOR_MDMM_APPLE_PGO_PROFILE}")
		message(FATAL_ERROR "GEARMULATOR_MDMM_APPLE_PGO_PROFILE must name an existing merged profile")
	endif()
	get_filename_component(_mdmm_profile "${GEARMULATOR_MDMM_APPLE_PGO_PROFILE}" ABSOLUTE)
	set(_mdmm_pgo_option "-fprofile-instr-use=${_mdmm_profile}")
	# Clang does not include the profile in its compiler dependency files.
	# Reconfigure when it changes, then change the private compile command so
	# every optimized object is rebuilt when new training replaces the file.
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_mdmm_profile}")
	file(SHA256 "${_mdmm_profile}" _mdmm_profile_sha256)
endif()

set(_mdmm_optimization_targets mdLib 68kEmu)
if(GEARMULATOR_MDMM_APPLE_OPTIMIZE_DSP)
	list(APPEND _mdmm_optimization_targets dsp56kEmu dsp56kBase)
endif()

foreach(_mdmm_target IN LISTS _mdmm_optimization_targets)
	target_compile_options(${_mdmm_target} PRIVATE "$<$<CONFIG:Release>:-flto=thin>")
	# Static libraries propagate the matching options to their final consumer.
	target_link_options(${_mdmm_target} INTERFACE "$<$<CONFIG:Release>:-flto=thin>")
	if(_mdmm_pgo_option)
		target_compile_options(${_mdmm_target} PRIVATE "$<$<CONFIG:Release>:${_mdmm_pgo_option}>")
		target_link_options(${_mdmm_target} INTERFACE "$<$<CONFIG:Release>:${_mdmm_pgo_option}>")
		if(GEARMULATOR_MDMM_APPLE_PGO_MODE STREQUAL "use")
			target_compile_definitions(${_mdmm_target} PRIVATE
				"$<$<CONFIG:Release>:GEARMULATOR_MDMM_PGO_PROFILE_SHA256=\"${_mdmm_profile_sha256}\">")
			# A mismatched profile is not a validated optimization build.
			target_compile_options(${_mdmm_target} PRIVATE
				"$<$<CONFIG:Release>:-Werror=profile-instr-out-of-date>")
		endif()
	endif()
endforeach()

message(STATUS "MD/MM Apple Release optimization: ThinLTO, PGO=${GEARMULATOR_MDMM_APPLE_PGO_MODE}, targets=${_mdmm_optimization_targets}")
