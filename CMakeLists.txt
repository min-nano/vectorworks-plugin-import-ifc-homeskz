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
		src/core/FeedbackSession.cpp
		src/core/Geometry.cpp
		src/core/ImportOptions.cpp
		src/core/Layout.cpp
		src/core/Progress.cpp
		src/core/Region.cpp
		src/core/Trace.cpp
		src/parse/BuildDocument.cpp
		src/parse/Context.cpp
		src/parse/Feedback.cpp
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

	# --- 殻の ID ---------------------------------------------------------------
	# **「アップデートに再起動が要るか」を決める鍵**（src/UpdaterParse.h の
	# NeedsRestartAfterInstall）。Vectorworks が読み込むのは殻だけなので、**本体
	# （.vwpayload）しか変わっていないなら置き換えて読み直すだけで済み、殻まで変わって
	# いれば再起動が要る**（src/PayloadAbi.h）。
	#
	# 値は**殻に入るものだけ**のハッシュにする。つまり draw/ や parse/ を直しても動かず、
	# 殻の登録・更新・境界（src/PayloadAbi.h）を直せば必ず動く——後者が肝で、版の食い違った
	# まま動かさないための歯止めになっている。
	#
	# 外部ツールを使わず CMake 組み込みのハッシュだけで出すのは、**mac と Windows の両方の
	# CI で同じ値が出ること**が要るため（片方でしか動かない道具を挟むと、そこが食い違った
	# 瞬間に「毎回再起動が要る」に化ける）。
	#
	# **改行を正規化してから混ぜる。** Windows のランナーは CRLF でチェックアウトするので、
	# 生のバイト列をそのままハッシュすると同じソースからプラットフォームごとに違う ID が
	# 出る（実測: 同じコミットの mac と win のリリースで値が食い違った）。判定は必ず同じ
	# プラットフォームの中で行われるので実害は無いが、**同じソースなら同じ ID** のほうが
	# 追いやすい。
	set(VW_SHELL_INPUTS
		CMakeLists.txt
		src/PluginPrefix.h
		src/BuildConfig.h
		src/Module-Info.plist.in
		src/ModuleMain.cpp
		src/PayloadAbi.h
		src/PayloadHost.h
		src/PayloadHost.cpp
		src/PayloadSession.h
		src/PayloadSession.cpp
		src/Updater.h
		src/Updater.cpp
		src/UpdaterFlow.cpp
		src/UpdaterHost.h
		src/UpdaterParse.h
		src/Extensions/ExtMenu.h
		src/Extensions/ExtMenu.cpp
		src/Extensions/ExtColumnMark.h
		src/Extensions/ExtColumnMark.cpp
		src/Extensions/ExtShearWall.h
		src/Extensions/ExtShearWall.cpp
		scripts/vw-update.sh
		scripts/vw-update.ps1
		scripts/vw-feedback.sh
		scripts/vw-feedback.ps1)
	# **`scripts/vw-install.*`（インストーラ）と `scripts/vw-uninstall.*`
	# （アンインストーラ）はここに入れない。** あれらは殻に入らず、
	# リリースの zip とアセットとして配られるもので、走るのは「落としてきた新しい版」の
	# ほう（scripts/vw-install.sh の冒頭）。ここへ足すと、配置の手順を直しただけで
	# 利用者に Vectorworks の再起動を強いることになる——それは、この仕組みが無くそうと
	# している手間そのもの。
	set(VW_SHELL_DIGEST_INPUT "")
	foreach(vw_shell_input IN LISTS VW_SHELL_INPUTS)
		if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${vw_shell_input}")
			message(FATAL_ERROR "VW_SHELL_INPUTS が指すファイルがありません: ${vw_shell_input}")
		endif()
		file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${vw_shell_input}" VW_SHELL_INPUT_TEXT)
		string(REPLACE "\r\n" "\n" VW_SHELL_INPUT_TEXT "${VW_SHELL_INPUT_TEXT}")
		string(SHA256 VW_SHELL_INPUT_DIGEST "${VW_SHELL_INPUT_TEXT}")
		string(APPEND VW_SHELL_DIGEST_INPUT "${vw_shell_input}:${VW_SHELL_INPUT_DIGEST}\n")
	endforeach()
	string(SHA256 vw_shell_digest "${VW_SHELL_DIGEST_INPUT}")
	string(SUBSTRING "${vw_shell_digest}" 0 12 VW_SHELL_ID)
	message(STATUS "殻の ID: ${VW_SHELL_ID}")

	# --- 殻の sources ------------------------------------------------------------
	# **Vectorworks が起動時に読み込むモジュールに入るもの。** メニューと 2 つの PIO の
	# 「登録」、自動アップデート、そして本体（ペイロード）の読み込み。**実処理は 1 行も
	# 入らない**——それが本体側にあることが、アップデートに Vectorworks の再起動を要らなく
	# している（src/PayloadAbi.h）。
	#
	# ここは SDK 非依存ライブラリ（HomeskzIfcCore）を**リンクしない**。殻は core/ も parse/
	# も呼ばないので、リンクしてしまうと「殻に入ってよいもの」の境界が曖昧になる。
	set(VW_SHELL_SOURCES
		src/ModuleMain.cpp
		src/PayloadHost.cpp
		src/PayloadSession.cpp
		src/Extensions/ExtMenu.cpp
		src/Extensions/ExtColumnMark.cpp
		src/Extensions/ExtShearWall.cpp
		src/Updater.cpp
		src/UpdaterFlow.cpp)

	# --- 本体（ペイロード）の sources ----------------------------------------------
	# **殻が dlopen / LoadLibrary で読み込む側。** SDK 依存の draw/ フェーズがまるごと
	# ここに入り、SDK 非依存の core/ + parse/ は HomeskzIfcCore として下でリンクする。
	# Vectorworks はこのモジュールの存在を知らないので、**入れ替えに再起動が要らない**。
	set(VW_PAYLOAD_SOURCES
		src/payload/PayloadMain.cpp
		src/draw/ImportCommand.cpp
		src/draw/Feedback.cpp
		src/draw/HostServices.cpp
		src/draw/ColumnMarkPio.cpp
		src/draw/ShearWallPio.cpp
		src/draw/DrawUtil.cpp
		src/draw/StructuralMember.cpp
		src/draw/ProgressDialog.cpp
		src/draw/ResultDialog.cpp
		src/draw/SettingsDialog.cpp
		src/draw/ExecuteDocument.cpp
		src/draw/Grid.cpp
		src/draw/Story.cpp
		src/draw/Floor.cpp
		src/draw/Footing.cpp
		src/draw/Member.cpp
		src/draw/Column.cpp
		src/draw/ColumnMark.cpp
		src/draw/ShearWall.cpp
		src/draw/Rafter.cpp
		src/draw/Roof.cpp
		src/draw/Symbol.cpp
		src/draw/Section.cpp
		src/draw/Sheet.cpp
		src/draw/Tag.cpp
		src/draw/Legend.cpp)

	# 殻と本体は**同じ SDK 設定でビルドする**（本体も libVWSDK.a を自分でリンクし、
	# GS_InitializeVCOM を自分で呼ぶ。src/payload/PayloadMain.cpp）。設定を 2 か所へ
	# 書き写すと片方だけ古くなるので、当てるところを 1 つにまとめてある。
	#
	#   vw_apply_sdk_module(<target> <sources...>)
	#
	# ここに入らないのは「殻か本体かで違うもの」だけ——出力の形（バンドル / .vlb /
	# .vwpayload）と、本体だけがリンクする HomeskzIfcCore。
	function(vw_apply_sdk_module target)
		set(VW_MODULE_SOURCES ${ARGN})

		set_target_properties(${target} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

		# Shared SDK include paths. The platform-specific "OnlyMac"/"OnlyWin" folder
		# is added in the per-platform blocks below.
		target_include_directories(
			${target}
			PRIVATE "${VW_SDK_INCLUDE}"
					"${VW_SDK_INCLUDE}/Kernel"
					"${VW_SDK_INCLUDE}/Interfaces"
					"${VW_SDK_INCLUDE}/VWMM"
					"${CMAKE_CURRENT_SOURCE_DIR}/src")

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
		# build stamp (commit + branch), shown in the dev build picker and the
		# diagnostics log. VW_SHELL_ID: **本体だけの更新で済むかを決める鍵**
		# （src/UpdaterParse.h）。
		#
		# **ビルドのたびに変わる値をここへ足さないこと**（時刻など）。この定義は殻と本体の
		# 全 TU に効くので、configure ごとに動く値を入れるとコンパイラキャッシュ（ccache）が
		# 毎回まるごと外れ、CI が丸ごと再ビルドになる。
		target_compile_definitions(
			${target}
			PRIVATE RELEASE_BLD=1 "VW_BUILD_VERSION=\"${VW_BUILD_VERSION}\""
					"VW_BUILD_BRANCH=\"${VW_BUILD_BRANCH}\"" "VW_SHELL_ID=\"${VW_SHELL_ID}\"")

		if(APPLE)
			# Build the sources as Objective-C++ (see note on project() above). Set
			# per target so each object is compiled with that target's definitions.
			set_source_files_properties(${VW_MODULE_SOURCES} TARGET_DIRECTORY ${target}
										PROPERTIES LANGUAGE OBJCXX)

			target_include_directories(${target} PRIVATE "${VW_SDK_INCLUDE}/OnlyMac")

			set_target_properties(${target} PROPERTIES OBJCXX_STANDARD 20)
			set_target_properties(${target} PROPERTIES OBJCXX_STANDARD_REQUIRED ON)

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
			target_include_directories(${target} PRIVATE "${VW_SDK_INCLUDE}/OnlyWin")

			set_target_properties(
				${target} PROPERTIES MSVC_RUNTIME_LIBRARY
									 "MultiThreadedDLL" # /MD, matches the SDK libs
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
	endfunction()

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

		# --- 殻 ------------------------------------------------------------------
		# MODULE: a plug-in loaded at run time, not linked against. On macOS this
		# becomes the bundle; on Windows it becomes a DLL (renamed to .vlb below).
		add_library(${target} MODULE ${VW_SHELL_SOURCES})
		set_target_properties(${target} PROPERTIES OUTPUT_NAME "${output_name}")
		vw_apply_sdk_module(${target} ${VW_SHELL_SOURCES})

		# --- 本体（ペイロード）-----------------------------------------------------
		# **Vectorworks が読み込むのは上の殻だけで、こちらは殻が自分で dlopen /
		# LoadLibrary する**——だから Vectorworks を再起動せずに入れ替えられる
		# （src/PayloadAbi.h）。
		#
		# 置き場所は**殻の隣**（mac もバンドルの中ではない）。mac のバンドルは署名が
		# リソースまで封をするので、中のファイルを差し替えると署名が壊れる。隣に置けば、
		# 本体を何度置き換えても殻の署名に触れない。拡張子 .vwpayload は Vectorworks に
		# プラグインとして拾わせないため（.vlb / .vwlibrary だと Plug-Ins の走査に
		# 引っかかり、殻と二重に読み込まれてしまう）。
		set(payload_target "${target}Payload")
		add_library(${payload_target} MODULE ${VW_PAYLOAD_SOURCES})
		#
		# ARCHIVE_OUTPUT_NAME: Windows では export を持つ DLL に**インポートライブラリ**が
		# 付いてくる。既定の名前は OUTPUT_NAME 由来なので、殻（`<name>.lib`）と本体で
		# **同じファイルを 2 つのターゲットが作る**ことになり衝突する。本体側だけ名前を
		# ずらす（実行時には使われないファイルで、配布物にも入らない）。
		set_target_properties(
			${payload_target}
			PROPERTIES OUTPUT_NAME "${output_name}"
					   ARCHIVE_OUTPUT_NAME "${output_name}Payload"
					   BUNDLE FALSE
					   PREFIX ""
					   SUFFIX ".vwpayload")
		vw_apply_sdk_module(${payload_target} ${VW_PAYLOAD_SOURCES})

		# The SDK-free Phase-1 code (core/ + parse/). **殻はこれをリンクしない**——
		# 解析も描画も本体の側にあるので、殻が core/ を引く理由が無い。
		target_link_libraries(${payload_target} PRIVATE HomeskzIfcCore)

		# 片方だけ作っても実機では動かない（殻は本体を隣から読む）。
		add_dependencies(${target} ${payload_target})

		if(ARG_DEV)
			target_compile_definitions(${target} PRIVATE VW_DEV_BUILD=1)
			target_compile_definitions(${payload_target} PRIVATE VW_DEV_BUILD=1)
		endif()

		if(APPLE)
			# Fill in a per-target Info.plist from the shared template. The channel
			# placeholder is deliberately NOT called VW_BUILD_CHANNEL: that name is
			# the cache option above, whose domain includes "both", while the plist
			# records the single channel THIS target belongs to ("stable"/"dev").
			set(VW_BUNDLE_NAME "${output_name}")
			set(VW_BUNDLE_ID "${bundle_id}")
			set(VW_PLUGIN_CHANNEL "${channel}")
			# VW_BUILD_BRANCH / VW_BUILD_VERSION / VW_SHELL_ID come from the parent scope.
			set(plist "${CMAKE_CURRENT_BINARY_DIR}/${target}-Info.plist")
			configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/Module-Info.plist.in" "${plist}" @ONLY)

			set_target_properties(
				${target}
				PROPERTIES BUNDLE TRUE
						   BUNDLE_EXTENSION "vwlibrary"
						   MACOSX_BUNDLE_INFO_PLIST "${plist}")
		else()
			# Windows: build a .vlb (a DLL) that self-exports plugin_module_main via
			# __declspec(dllexport) (see GS_EXTERNAL_ENTRY in the SDK's Config.h).
			set_target_properties(${target} PROPERTIES PREFIX "" SUFFIX ".vlb")
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

		# --- Bundle the scripts the installed plug-in drives ----------------------
		# See src/Updater.cpp. macOS reads Contents/Resources/<name>.sh from inside
		# the bundle; Windows reads <name>.ps1 from next to the .vlb.
		#
		#   vw-update    自動アップデート（殻が起動時に走らせる）
		#   vw-feedback  実機フィードバックの投稿（本体が殻越しに走らせる。M23）
		if(APPLE)
			add_custom_command(
				TARGET ${target}
				POST_BUILD
				COMMAND "${CMAKE_COMMAND}" -E make_directory
						"$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Resources"
				COMMAND
					"${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/scripts/vw-update.sh"
					"$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Resources/vw-update.sh"
				COMMAND
					"${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/scripts/vw-feedback.sh"
					"$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Resources/vw-feedback.sh"
				VERBATIM
				COMMENT "Bundling vw-update.sh / vw-feedback.sh into ${output_name}")
		else()
			# Windows has no Info.plist, so stamp the build commit into a sidecar
			# "<name>.commit" file next to the module. The updater reads it to tell
			# which build is installed (the mac side reads VWBuildCommit from the
			# plist instead). Written at configure time from the parent-scope stamp.
			set(commit_file "${CMAKE_CURRENT_BINARY_DIR}/${output_name}.commit")
			file(WRITE "${commit_file}" "${VW_BUILD_VERSION}\n")
			# 殻の ID も同じやり方で控える。**「本体だけ入れ替えれば済むか」をアップデータが
			# 判断する材料**で、mac 側は Info.plist の VWShellId から読む
			# （scripts/vw-update.ps1 の Get-InstalledShellId と対）。
			set(shell_id_file "${CMAKE_CURRENT_BINARY_DIR}/${output_name}.shell-id")
			file(WRITE "${shell_id_file}" "${VW_SHELL_ID}\n")
			add_custom_command(
				TARGET ${target}
				POST_BUILD
				COMMAND
					"${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/scripts/vw-update.ps1"
					"$<TARGET_FILE_DIR:${target}>/vw-update.ps1"
				COMMAND
					"${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/scripts/vw-feedback.ps1"
					"$<TARGET_FILE_DIR:${target}>/vw-feedback.ps1"
				COMMAND "${CMAKE_COMMAND}" -E copy "${commit_file}"
						"$<TARGET_FILE_DIR:${target}>/${output_name}.commit"
				COMMAND "${CMAKE_COMMAND}" -E copy "${shell_id_file}"
						"$<TARGET_FILE_DIR:${target}>/${output_name}.shell-id"
				VERBATIM
				COMMENT
					"Bundling vw-update.ps1 / vw-feedback.ps1 and build stamps next to ${output_name}"
			)
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
