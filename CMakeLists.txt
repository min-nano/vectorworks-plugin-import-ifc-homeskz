cmake_minimum_required(VERSION 3.20)

#
# HomeskzIfcImport - the ホームズ君 IFC import plug-in for Vectorworks 2026.
#
# The build produces a loadable plug-in that adds a single menu command; running
# it picks an IFC file and draws the model it describes (see README.md). It
# builds on BOTH platforms Vectorworks 2026 supports, from the same sources:
#
#   * macOS   -> "HomeskzIfcImport.vwlibrary" bundle (the .vwr resource lives inside
#                the bundle at Contents/Resources).
#   * Windows -> "HomeskzIfcImport.vlb" (a DLL) with a sibling "HomeskzIfcImport.vwr"
#                resource file next to it, as the SDK expects on Windows.
#
# The plug-in identifiers (target, output name, bundle id, menu category) are
# documented in README.md ("プラグイン識別子"); this project derives them from the
# vectorworks-plugin-native-template it was ported from.
#
# Requirements:
#   macOS:
#     * Xcode command line tools (Xcode 16.2 is the version Vectorworks 2026
#       officially targets).
#     * The Vectorworks 2026 mac SDK (folder that CONTAINS "SDKLib").
#   Windows:
#     * Visual Studio 2022 (v143 toolset), x64.
#     * The Vectorworks 2026 win SDK (folder that CONTAINS "SDKLib").
#
# Point CMake at the SDK folder that CONTAINS "SDKLib" via -DVW_SDK_DIR=/path
# (or the VW_SDK_DIR environment variable). Example:
#   macOS:   cmake -S . -B build -DVW_SDK_DIR=/path/to/2026-NNA-eng-mac-SDK
#            cmake --build build --config Release
#   Windows: cmake -S . -B build -DVW_SDK_DIR=C:/path/to/2026-NNA-eng-win-SDK
#            cmake --build build --config Release
#

# --- Architecture / deployment target (macOS only; set before project()) ----
# Default to Apple Silicon. Override with e.g.
#   -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"  for a universal binary.
# Guarded to the mac host so these cache variables aren't created on Windows.
if(CMAKE_HOST_APPLE)
	if(NOT DEFINED CMAKE_OSX_ARCHITECTURES)
		set(CMAKE_OSX_ARCHITECTURES
			"arm64"
			CACHE STRING "Target architectures")
	endif()
	if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
		set(CMAKE_OSX_DEPLOYMENT_TARGET
			"14.0"
			CACHE STRING "Minimum macOS version")
	endif()
endif()

# On macOS the SDK headers are compiled as Objective-C++ (some mac headers
# #import CoreFoundation), matching how the SDK's own Xcode projects build. On
# Windows they are plain C++. So enable OBJCXX only where it exists.
project(HomeskzIfcImport LANGUAGES CXX)
if(APPLE)
	enable_language(OBJCXX)
endif()

# --- Build options -----------------------------------------------------------
# The loadable plug-in (needs the Vectorworks SDK; macOS/Windows only) and the
# unit tests are independent build products:
#   * VW_BUILD_PLUGIN (default ON)  -> the .vwlibrary / .vlb plug-in.
#   * VW_BUILD_TESTS  (default OFF) -> the SDK-free unit tests (src/core/ +
#                                      src/parse/ and the updater logic).
#                                      These build on ANY platform, so the coverage
#                                      job can run them on a plain Linux runner
#                                      with -DVW_BUILD_PLUGIN=OFF -DVW_BUILD_TESTS=ON.
#   * VW_ENABLE_COVERAGE (default OFF) -> add gcov instrumentation to the tests.
#   * VW_ENABLE_SANITIZERS (default OFF) -> build/run the tests under
#                                      AddressSanitizer + UndefinedBehaviorSanitizer
#                                      so out-of-bounds accesses, use-after-free,
#                                      leaks, and undefined behaviour fail the test
#                                      run. Catches memory-safety regressions from
#                                      refactoring and mishandling of unexpected
#                                      external input (e.g. the updater parser).
option(VW_BUILD_PLUGIN "Build the Vectorworks plug-in (requires the VW SDK)" ON)
option(VW_BUILD_TESTS "Build the unit tests" OFF)
option(VW_ENABLE_COVERAGE "Instrument the tests for code coverage (gcov/llvm-cov)" OFF)
option(VW_ENABLE_SANITIZERS "Build/run the tests under AddressSanitizer + UBSan" OFF)

# Which plug-in channel(s) to build: "stable", "dev", or "both".
#
# The stable and dev plug-ins share every source file and differ only by the
# VW_DEV_BUILD define, yet each build compiles the SDK-heavy translation units,
# links, (on macOS) code-signs and packages a bundle independently. On any one
# CI run only ONE channel is ever published — main publishes stable, a pull
# request publishes dev — so building the other is pure waste. CI therefore
# passes -DVW_BUILD_CHANNEL to build just the channel it will publish, halving
# that per-target work. Local builds default to "both" so nothing changes for
# developers, who may want to load both plug-ins side by side.
set(VW_BUILD_CHANNEL
	"both"
	CACHE STRING "Which plug-in channel(s) to build: stable, dev, or both")
set_property(CACHE VW_BUILD_CHANNEL PROPERTY STRINGS stable dev both)

# Precompiled header for the SDK umbrella (PluginPrefix.h). ON for real builds —
# it is the biggest compile-time win. Turned OFF only when generating the
# compile_commands.json that clang-tidy consumes: clang-tidy analyses the sources
# WITHOUT running the build, so the .pch is never produced and a compile database
# carrying "-include-pch <missing>.pch" would make every file error out. A
# PCH-free database gives clang-tidy the identical flags minus that include.
option(VW_ENABLE_PCH "Precompile PluginPrefix.h for the plug-in targets" ON)

if(VW_BUILD_PLUGIN
   AND NOT APPLE
   AND NOT WIN32)
	message(
		FATAL_ERROR
			"The Vectorworks plug-in can only be built on macOS or Windows. To build "
			"just the unit tests on this platform, configure with "
			"-DVW_BUILD_PLUGIN=OFF -DVW_BUILD_TESTS=ON.")
endif()

# --- SDK-free core/parse library ---------------------------------------------
# The 2-phase architecture (CLAUDE.md / docs/DEV-NOTES.md) keeps IFC parsing
# (src/parse/) and the phase-independent foundation (src/core/) STRICTLY free of
# the Vectorworks SDK, so they compile, run and unit-test on any toolchain — the
# same footing as the updater's SDK-free logic. This library is that code, built
# once as a single target that both the plug-in and the unit tests link against:
#
#   * the plug-in (VW_BUILD_PLUGIN) links it and adds the SDK-dependent draw/
#     phase (src/draw/) on top;
#   * the tests (VW_BUILD_TESTS) link it to exercise core/parse WITHOUT the SDK,
#     which is what lets the SDK-free Linux jobs build it with no SDK present.
#
# The dependency direction is enforced by construction: these sources include
# only core/ and parse/ headers (never PluginPrefix.h / the SDK). The draw/ phase
# is compiled into the plug-in target instead (it needs the SDK), and the two
# phases meet only through core/Document.h.
if(VW_BUILD_PLUGIN OR VW_BUILD_TESTS)
	add_library(
		HomeskzIfcCore STATIC
		src/core/Document.cpp
		src/core/Foundation.cpp
		src/core/Geometry.cpp
		src/core/Layout.cpp
		src/core/PolygonBool.cpp
		src/core/Progress.cpp
		src/core/Region.cpp
		src/core/Trace.cpp
		src/parse/BuildDocument.cpp
		src/parse/Context.cpp
		src/parse/Step.cpp
		src/parse/Loader.cpp
		src/parse/Summary.cpp
		src/parse/Grid.cpp
		src/parse/Story.cpp
		src/parse/Floor.cpp
		src/parse/Footing.cpp
		src/parse/Member.cpp
		src/parse/Column.cpp
		src/parse/ColumnMark.cpp
		src/parse/Noboribari.cpp
		src/parse/AnchorBolt.cpp
		src/parse/FloorPost.cpp
		src/parse/FireBrace.cpp
		src/parse/Joint.cpp
		src/parse/Rafter.cpp
		src/parse/Roof.cpp
		src/parse/Section.cpp
		src/parse/ShearWall.cpp
		src/parse/Sheet.cpp
		src/parse/Tag.cpp
		src/parse/StructuralClass.cpp
		src/parse/IfcGeometry.cpp)
	target_compile_features(HomeskzIfcCore PUBLIC cxx_std_20)
	# PUBLIC so every consumer (plug-in + tests) resolves "core/..." / "parse/..."
	# includes against src/ the same way.
	target_include_directories(HomeskzIfcCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

	# Our own SDK-free code is kept warning-clean, so warnings are errors here
	# (unlike the plug-in target, whose SDK headers are not warning-clean).
	if(MSVC)
		target_compile_options(HomeskzIfcCore PRIVATE /W4 /WX)
	else()
		target_compile_options(HomeskzIfcCore PRIVATE -Wall -Wextra -Werror)
	endif()

	# Match the tests' instrumentation so, when the coverage or sanitizer job
	# builds the tests, this library's code is measured / checked too (the test
	# executables carry the corresponding link flags; see tests/CMakeLists.txt).
	if(VW_ENABLE_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
		target_compile_options(
			HomeskzIfcCore
			PRIVATE -O0
					-g
					--coverage
					-fprofile-arcs
					-ftest-coverage)
	endif()
	if(VW_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
		target_compile_options(
			HomeskzIfcCore PRIVATE -g -fno-omit-frame-pointer -fsanitize=address,undefined
								   -fno-sanitize-recover=all)
	endif()
endif()

if(VW_BUILD_PLUGIN)

	# --- Locate the Vectorworks SDK ---------------------------------------------
	if(NOT DEFINED VW_SDK_DIR AND DEFINED ENV{VW_SDK_DIR})
		set(VW_SDK_DIR "$ENV{VW_SDK_DIR}")
	endif()

	if(NOT DEFINED VW_SDK_DIR)
		message(
			FATAL_ERROR
				"VW_SDK_DIR is not set. Point it at the folder that contains 'SDKLib', e.g.\n"
				"  cmake -S . -B build -DVW_SDK_DIR=/path/to/2026-NNA-eng-<mac|win>-SDK")
	endif()

	set(VW_SDK_INCLUDE "${VW_SDK_DIR}/SDKLib/Include")

	if(NOT EXISTS "${VW_SDK_INCLUDE}/VectorworksSDK.h")
		message(FATAL_ERROR "Could not find VectorworksSDK.h under '${VW_SDK_INCLUDE}'.\n"
							"Check VW_SDK_DIR (currently '${VW_SDK_DIR}').")
	endif()

	# Per-platform library + resource-tool locations. Each platform's SDK ships a
	# different static library and a different BuildVWR tool.
	if(APPLE)
		set(VW_SDK_LIB "${VW_SDK_DIR}/SDKLib/LibMac/Release")
		if(NOT EXISTS "${VW_SDK_LIB}/libVWSDK.a")
			message(FATAL_ERROR "Could not find libVWSDK.a under '${VW_SDK_LIB}'.")
		endif()
		# BuildVWR packages resources/<name>.vwr into the bundle.
		set(VW_BUILDVWR "${VW_SDK_DIR}/SDKLib/ToolsMac/BuildVWR/BuildVWR")
	else()
		# Windows is x64-only. The SDK ships only x64 libraries (LibWin holds an
		# x64 VWSDK.lib / VWMM.lib — there is no ARM64 variant), and Vectorworks for
		# Windows itself is an x64 application. A plug-in DLL must match the host
		# process, so x64 is the only valid target — build with -A x64. This still
		# runs on Windows-on-ARM: there the x64 Vectorworks runs under the OS's x64
		# emulation and loads this x64 plug-in as-is (a native ARM64 plug-in could
		# not load into that emulated x64 host, and could not be linked anyway).
		if(CMAKE_GENERATOR_PLATFORM AND NOT CMAKE_GENERATOR_PLATFORM MATCHES "^([xX]64|Win32)$")
			message(WARNING "Building for platform '${CMAKE_GENERATOR_PLATFORM}', but the "
							"Vectorworks SDK provides x64 libraries only. Configure with -A x64.")
		endif()
		set(VW_SDK_LIB "${VW_SDK_DIR}/SDKLib/LibWin/Release")
		if(NOT EXISTS "${VW_SDK_LIB}/VWSDK.lib")
			message(FATAL_ERROR "Could not find VWSDK.lib under '${VW_SDK_LIB}'.")
		endif()
		# buildvwr.exe writes <name>.vwr next to the module. It shells out to 7-Zip,
		# whose path is handed to it via the VWR_7ZCMD environment variable.
		set(VW_BUILDVWR "${VW_SDK_DIR}/SDKLib/ToolsWin/BuildVWR/buildvwr.exe")
		set(VW_BUILDVWR_7Z "${VW_SDK_DIR}/SDKLib/ToolsWin/BuildVWR/7z")
	endif()

	if(NOT EXISTS "${VW_BUILDVWR}")
		message(WARNING "BuildVWR tool not found at '${VW_BUILDVWR}'. The plug-ins will build, "
						"but their menu commands will have no display name. Ensure the SDK's "
						"ToolsMac/ToolsWin folder is present in VW_SDK_DIR.")
	endif()

	# --- Build stamp -------------------------------------------------------------
	# Short identifier of this exact build (git commit in CI, "local" otherwise).
	# Stamped into each build (mac: Info.plist, win: a "<name>.commit" sidecar) and
	# compiled in, so an installed build can be identified and compared by the
	# updater scripts and named in the dev build picker.
	if(NOT DEFINED VW_BUILD_VERSION)
		set(VW_BUILD_VERSION
			"local"
			CACHE STRING "Short build identifier (git commit)")
	endif()

	# Git branch this build came from ("main" for stable, the feature/PR branch for
	# a dev build, "local" otherwise). CI passes it via -DVW_BUILD_BRANCH; shown in
	# the dev build picker next to the commit.
	if(NOT DEFINED VW_BUILD_BRANCH)
		set(VW_BUILD_BRANCH
			"local"
			CACHE STRING "Git branch this build came from")
	endif()

	# The plug-in sources: the module entry point + menu command + updater, plus
	# the SDK-dependent draw/ phase (src/draw/). The SDK-free core/ + parse/ code
	# is NOT listed here — it comes in via the HomeskzIfcCore library linked below.
	set(VW_COMMON_SOURCES
		src/ModuleMain.cpp
		src/Extensions/ExtMenu.cpp
		src/Updater.cpp
		src/UpdaterFlow.cpp
		src/draw/DrawUtil.cpp
		src/draw/StructuralMember.cpp
		src/draw/ProgressDialog.cpp
		src/draw/ResultDialog.cpp
		src/draw/ExecuteDocument.cpp
		src/draw/Grid.cpp
		src/draw/Story.cpp
		src/draw/Floor.cpp
		src/draw/Footing.cpp
		src/draw/Member.cpp
		src/draw/Column.cpp
		src/draw/ColumnMark.cpp
		src/Extensions/ExtColumnMark.cpp
		src/draw/ShearWall.cpp
		src/Extensions/ExtShearWall.cpp
		src/Extensions/ExtFoundation.cpp
		src/draw/Rafter.cpp
		src/draw/Roof.cpp
		src/draw/Symbol.cpp
		src/draw/Section.cpp
		src/draw/Sheet.cpp
		src/draw/Tag.cpp
		src/draw/Legend.cpp)

	# add_vw_plugin(<target> <output-name> <bundle-id> <channel> [DEV])
	#
	# Defines one loadable plug-in ("<output-name>.vwlibrary" bundle on macOS,
	# "<output-name>.vlb" module on Windows). The same sources build both the stable
	# and dev plug-ins; passing DEV compiles them with VW_DEV_BUILD so the plug-in
	# takes on its dev identity (see src/BuildConfig.h). Each plug-in packages its
	# own "<output-name>.vwr" resource, whose base name must match the identifier
	# returned by DefaultPluginVWRIdentifier().
	function(
		add_vw_plugin
		target
		output_name
		bundle_id
		channel)
		cmake_parse_arguments(
			ARG
			"DEV"
			""
			""
			${ARGN})

		# MODULE: a plug-in loaded at run time, not linked against. On macOS this
		# becomes the bundle; on Windows it becomes a DLL (renamed to .vlb below).
		add_library(${target} MODULE ${VW_COMMON_SOURCES})

		set_target_properties(
			${target}
			PROPERTIES CXX_STANDARD 20
					   CXX_STANDARD_REQUIRED ON
					   OUTPUT_NAME "${output_name}")

		# Shared SDK include paths. The platform-specific "OnlyMac"/"OnlyWin" folder
		# is added in the per-platform blocks below.
		target_include_directories(
			${target}
			PRIVATE "${VW_SDK_INCLUDE}"
					"${VW_SDK_INCLUDE}/Kernel"
					"${VW_SDK_INCLUDE}/Interfaces"
					"${VW_SDK_INCLUDE}/VWMM"
					"${CMAKE_CURRENT_SOURCE_DIR}/src")

		# The SDK-free Phase-1 code (core/ + parse/). draw/ (compiled into this
		# target above) reaches parse output through core/Document.h.
		target_link_libraries(${target} PRIVATE HomeskzIfcCore)

		# Precompiled header. Every SDK-dependent TU begins with PluginPrefix.h,
		# which pulls in the whole Vectorworks SDK umbrella header — by far the
		# largest thing each TU parses. Compiling it once as a PCH (per target,
		# per language) instead of re-parsing it in every TU is the single biggest
		# reduction in the SDK build's compile time. CMake generates the PCH in the
		# target's own language, so on macOS (where these sources are OBJCXX) it is
		# built as Objective-C++ and on Windows as C++ — matching how each TU is
		# compiled. This also cooperates with a compiler-cache launcher (ccache):
		# with the SDK header behind a stable PCH, unchanged TUs hit the cache.
		# Skipped for the clang-tidy compile database (see VW_ENABLE_PCH).
		if(VW_ENABLE_PCH)
			target_precompile_headers(${target} PRIVATE
									  "${CMAKE_CURRENT_SOURCE_DIR}/src/PluginPrefix.h")
		endif()

		# RELEASE_BLD: link against the Release SDK. VW_BUILD_VERSION/VW_BUILD_BRANCH:
		# build stamp (commit + branch), shown in the menu command's alert.
		target_compile_definitions(
			${target} PRIVATE RELEASE_BLD=1 "VW_BUILD_VERSION=\"${VW_BUILD_VERSION}\""
							  "VW_BUILD_BRANCH=\"${VW_BUILD_BRANCH}\"")
		if(ARG_DEV)
			target_compile_definitions(${target} PRIVATE VW_DEV_BUILD=1)
		endif()

		if(APPLE)
			# Build the sources as Objective-C++ (see note on project() above). Set
			# per target so each object is compiled with that target's definitions.
			set_source_files_properties(${VW_COMMON_SOURCES} TARGET_DIRECTORY ${target}
										PROPERTIES LANGUAGE OBJCXX)

			target_include_directories(${target} PRIVATE "${VW_SDK_INCLUDE}/OnlyMac")

			# Fill in a per-target Info.plist from the shared template. The channel
			# placeholder is deliberately NOT called VW_BUILD_CHANNEL: that name is
			# the cache option above, whose domain includes "both", while the plist
			# records the single channel THIS target belongs to ("stable"/"dev").
			set(VW_BUNDLE_NAME "${output_name}")
			set(VW_BUNDLE_ID "${bundle_id}")
			set(VW_PLUGIN_CHANNEL "${channel}")
			# VW_BUILD_BRANCH and VW_BUILD_VERSION come from the parent scope (CI/-D).
			set(plist "${CMAKE_CURRENT_BINARY_DIR}/${target}-Info.plist")
			configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/Module-Info.plist.in" "${plist}" @ONLY)

			set_target_properties(
				${target}
				PROPERTIES OBJCXX_STANDARD 20
						   OBJCXX_STANDARD_REQUIRED ON
						   BUNDLE TRUE
						   BUNDLE_EXTENSION "vwlibrary"
						   MACOSX_BUNDLE_INFO_PLIST "${plist}")

			target_compile_options(${target} PRIVATE -stdlib=libc++ -Wno-deprecated-declarations)

			# Link the static SDK library plus the frameworks it depends on. VWMM is a
			# framework shipped inside the SDK; the others are system frameworks.
			target_link_libraries(
				${target}
				PRIVATE "${VW_SDK_LIB}/libVWSDK.a"
						"-F${VW_SDK_LIB}"
						"-framework VWMM"
						"-framework Carbon"
						"-framework Cocoa")
		else()
			# Windows: build a .vlb (a DLL) that self-exports plugin_module_main via
			# __declspec(dllexport) (see GS_EXTERNAL_ENTRY in the SDK's Config.h).
			target_include_directories(${target} PRIVATE "${VW_SDK_INCLUDE}/OnlyWin")

			set_target_properties(
				${target}
				PROPERTIES PREFIX ""
						   SUFFIX ".vlb"
						   MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" # /MD, matches the SDK libs
			)

			# _WINDOWS switches the SDK headers into their GS_WIN configuration
			# (Config.h). MSVC does not define it on its own, so we must. The rest
			# mirror the SDK's own plug-in build settings (NNA_PluginBuild props).
			target_compile_definitions(
				${target}
				PRIVATE _WINDOWS
						WIN32
						_USRDLL
						_WINDLL
						_WIN_EXTERNAL_
						UNICODE
						_UNICODE
						NDEBUG
						_CRT_SECURE_NO_WARNINGS)

			# /bigobj: the template-heavy SDK headers overflow the default section
			# limit. /wd4996: silence deprecation warnings (parallels the mac flag).
			target_compile_options(${target} PRIVATE /EHsc /bigobj /wd4996)

			# /MP: compile this target's translation units in parallel (MSVC is
			# otherwise single-file-at-a-time within a target); the SDK-heavy TUs
			# dominate the build, so this is a large, free win on multi-core runners.
			# Only for the real MSVC build: CMake's MSVC variable is also true for
			# clang-cl (used by the clang-tidy compile database), where /MP is an
			# unused argument that warnings-as-errors would reject.
			if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
				target_compile_options(${target} PRIVATE /MP)
			endif()

			# The host Vectorworks.exe provides the actual SDK functions through the
			# callback pointer (GS_InitializeVCOM), so we link only the SDK's static
			# library, the VWMM import library, and the two Windows system libs the
			# SDK references.
			target_link_libraries(${target} PRIVATE "${VW_SDK_LIB}/VWSDK.lib"
													"${VW_SDK_LIB}/VWMM.lib" USP10.lib Rpcrt4.lib)
		endif()

		# --- Package "<output-name>.vwr" -----------------------------------------
		# The SDK's BuildVWR tool reads resources/<output-name>.vwr and, in Release
		# mode, writes a no-compression zip. On macOS it goes inside the bundle at
		# Contents/Resources; on Windows it sits next to the .vlb module.
		set(vwr_src "${CMAKE_CURRENT_SOURCE_DIR}/resources/${output_name}.vwr")
		if(EXISTS "${VW_BUILDVWR}")
			if(APPLE)
				add_custom_command(
					TARGET ${target}
					POST_BUILD
					COMMAND "${CMAKE_COMMAND}" -E make_directory
							"$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Resources"
					COMMAND
						"${CMAKE_COMMAND}" -E env "VWR_INPUT=${vwr_src}"
						"VWR_OUTPUT=$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Resources/${output_name}.vwr"
						"VWR_MODE=Release" "${VW_BUILDVWR}"
					VERBATIM
					COMMENT "Packaging ${output_name}.vwr resource into the bundle")
			else()
				add_custom_command(
					TARGET ${target}
					POST_BUILD
					COMMAND
						"${CMAKE_COMMAND}" -E env "VWR_INPUT=${vwr_src}"
						"VWR_OUTPUT=$<TARGET_FILE_DIR:${target}>/${output_name}.vwr"
						"VWR_MODE=Release" "VWR_7ZCMD=${VW_BUILDVWR_7Z}" "${VW_BUILDVWR}"
					VERBATIM
					COMMENT "Packaging ${output_name}.vwr resource next to the module")
			endif()
		endif()

		# --- Bundle the updater so the installed plug-in can update itself --------
		# See src/Updater.cpp. macOS reads Contents/Resources/vw-update.sh from
		# inside the bundle; Windows reads vw-update.ps1 from next to the .vlb.
		if(APPLE)
			add_custom_command(
				TARGET ${target}
				POST_BUILD
				COMMAND "${CMAKE_COMMAND}" -E make_directory
						"$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Resources"
				COMMAND
					"${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/scripts/vw-update.sh"
					"$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Resources/vw-update.sh"
				VERBATIM
				COMMENT "Bundling vw-update.sh into ${output_name}")
		else()
			# Windows has no Info.plist, so stamp the build commit into a sidecar
			# "<name>.commit" file next to the module. The updater reads it to tell
			# which build is installed (the mac side reads VWBuildCommit from the
			# plist instead). Written at configure time from the parent-scope stamp.
			set(commit_file "${CMAKE_CURRENT_BINARY_DIR}/${output_name}.commit")
			file(WRITE "${commit_file}" "${VW_BUILD_VERSION}\n")
			add_custom_command(
				TARGET ${target}
				POST_BUILD
				COMMAND
					"${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/scripts/vw-update.ps1"
					"$<TARGET_FILE_DIR:${target}>/vw-update.ps1"
				COMMAND "${CMAKE_COMMAND}" -E copy "${commit_file}"
						"$<TARGET_FILE_DIR:${target}>/${output_name}.commit"
				VERBATIM
				COMMENT "Bundling vw-update.ps1 and build stamp next to ${output_name}")
		endif()
	endfunction()

	# Stable plug-in (from main) and dev plug-in (from feature/PR branches). Both
	# can be installed and loaded at the same time. VW_BUILD_CHANNEL selects which
	# to build; CI narrows this to the single channel it will publish (see the
	# option's definition above), while a local "both" builds the pair.
	if(NOT VW_BUILD_CHANNEL MATCHES "^(stable|dev|both)$")
		message(FATAL_ERROR "VW_BUILD_CHANNEL must be 'stable', 'dev' or 'both' "
							"(got '${VW_BUILD_CHANNEL}').")
	endif()

	if(VW_BUILD_CHANNEL STREQUAL "stable" OR VW_BUILD_CHANNEL STREQUAL "both")
		add_vw_plugin(HomeskzIfcImport "HomeskzIfcImport" "io.github.min-nano.HomeskzIfcImport"
					  "stable")
	endif()
	if(VW_BUILD_CHANNEL STREQUAL "dev" OR VW_BUILD_CHANNEL STREQUAL "both")
		add_vw_plugin(
			HomeskzIfcImportDev
			"HomeskzIfcImportDev"
			"io.github.min-nano.HomeskzIfcImportDev"
			"dev"
			DEV)
	endif()

endif() # VW_BUILD_PLUGIN

# --- Unit tests --------------------------------------------------------------
# SDK-free tests: the import's parse side (src/core/ + src/parse/, via the
# HomeskzIfcCore library) and the updater's parsing/quoting/path helpers. They
# build on any platform, so a fast Linux job can run them and measure coverage.
if(VW_BUILD_TESTS)
	enable_testing()
	add_subdirectory(tests)
endif()
