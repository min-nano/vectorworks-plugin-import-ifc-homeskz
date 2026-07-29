# Unit tests for the plug-in's SDK-independent code. Two executables:
#
#   * UpdaterParseTests — the pure parsing/quoting/path/decision helpers in
#                         src/UpdaterParse.h (header-only).
#   * UpdaterFlowTests  — the update FLOWS in src/UpdaterFlow.cpp, driven through
#                         a fake IUpdaterHost so the whole flow (branches + dialog
#                         wording) is exercised without the SDK.
#
# Neither links the Vectorworks SDK, so they build and run on any toolchain —
# including a plain Linux runner — which is what makes the coverage job cheap.
# Enable with -DVW_BUILD_TESTS=ON (see the top-level CMakeLists.txt);
# VW_ENABLE_COVERAGE adds gcov instrumentation.

# Coverage instrumentation (gcov/llvm-cov compatible), applied per test target so
# the numbers reflect the code under test, not the harness build.
if(VW_ENABLE_COVERAGE AND NOT (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
	message(FATAL_ERROR "VW_ENABLE_COVERAGE requires GCC or Clang.")
endif()

# AddressSanitizer + UndefinedBehaviorSanitizer. Applied per test target (the
# harness itself is tiny, so instrumenting it too costs nothing). With ASan a
# heap/stack overflow or use-after-free aborts the run; with UBSan any undefined
# behaviour does the same, and -fno-sanitize-recover=all makes even "recoverable"
# UBSan reports fatal so CI turns red instead of merely logging. -g keeps the
# frame info that makes the sanitizer reports point at a source line, and
# -fno-omit-frame-pointer keeps the stack traces readable.
if(VW_ENABLE_SANITIZERS AND NOT (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
	message(FATAL_ERROR "VW_ENABLE_SANITIZERS requires GCC or Clang.")
endif()

# Whether the bundled-updater-script tests are REQUIRED to run (CI) instead of
# being skipped when their interpreter or a helper tool is missing. OFF for a
# plain local build, so a developer without pwsh (or the macOS back-end deps)
# still gets a clean `ctest`. The Tests workflow turns it ON, which makes a
# missing interpreter a configure-time error and tells each harness to FAIL
# (not skip) on a missing tool — so the script tests can never silently pass
# without actually running in CI.
option(VW_REQUIRE_SCRIPT_TESTS
	   "Fail rather than skip the updater-script tests when a tool is missing" OFF)

function(vw_add_test name)
	add_executable(${name} ${ARGN})
	target_compile_features(${name} PRIVATE cxx_std_20)
	# Tests include the sources under "src" and the local "TestFramework.h".
	target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}/src"
											   "${CMAKE_CURRENT_SOURCE_DIR}")
	# Warnings are errors for the test build: any new warning fails CI rather than
	# scrolling past unnoticed. MSVC uses /W4 /WX; GCC/Clang use -Wall -Wextra
	# -Werror. Applied per test target so it never leaks into the SDK plug-in
	# build (whose SDK headers are not warning-clean).
	if(MSVC)
		target_compile_options(${name} PRIVATE /W4 /WX)
	else()
		target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
	endif()
	if(VW_ENABLE_COVERAGE)
		target_compile_options(
			${name}
			PRIVATE -O0
					-g
					--coverage
					-fprofile-arcs
					-ftest-coverage)
		target_link_options(${name} PRIVATE --coverage)
	endif()
	if(VW_ENABLE_SANITIZERS)
		target_compile_options(
			${name} PRIVATE -g -fno-omit-frame-pointer -fsanitize=address,undefined
							-fno-sanitize-recover=all)
		target_link_options(${name} PRIVATE -fsanitize=address,undefined)
	endif()
	add_test(NAME ${name} COMMAND ${name})
endfunction()

# Header-only helpers.
vw_add_test(UpdaterParseTests UpdaterParseTests.cpp)

# The flows, compiled with the fake host. src/UpdaterFlow.cpp includes no SDK
# header, so it links straight into the test binary.
vw_add_test(UpdaterFlowTests UpdaterFlowTests.cpp "${CMAKE_SOURCE_DIR}/src/UpdaterFlow.cpp")

# Robustness / adversarial-input tests: feed the parsers unexpected external
# input (malformed script output, embedded NULs, huge/degenerate lines, random
# byte soup) to prove they never read out of bounds or invoke undefined
# behaviour. Most valuable under VW_ENABLE_SANITIZERS, where a mistake aborts;
# they also guard against a refactor quietly breaking the parsing contract.
vw_add_test(UpdaterRobustnessTests UpdaterRobustnessTests.cpp)

# Smoke test for the SDK-free 2-phase skeleton (src/core/ + src/parse/). It links
# the HomeskzIfcCore library (defined in the top-level CMakeLists.txt) instead of
# compiling those sources directly, proving the CMake target split builds and
# links with no SDK present. As parse modules land, their real tests (ParseGrid,
# Geometry, …) join this one and link the same library.
vw_add_test(CoreDocumentTests CoreDocumentTests.cpp)
target_link_libraries(CoreDocumentTests PRIVATE HomeskzIfcCore)

# Minimal STEP reader (src/parse/Step): tokeniser + entity graph exercised on
# small synthetic STEP snippets (references, numbers, strings, enums, nested
# lists, typed values, referrers/resolve, determinism). SDK-free.
vw_add_test(StepTests StepTests.cpp)
target_link_libraries(StepTests PRIVATE HomeskzIfcCore)

# IFC loader + sanitiser (src/parse/Loader): text-level IFCFOOTINGTYPE removal and
# real-fixture file loading. HOMESKZ_FIXTURES_DIR points the file-based cases at
# the checked-in fixtures (tests/fixtures/), reused as parse modules land.
vw_add_test(LoaderTests LoaderTests.cpp)
target_link_libraries(LoaderTests PRIVATE HomeskzIfcCore)
target_compile_definitions(LoaderTests
						   PRIVATE "HOMESKZ_FIXTURES_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\"")

# Grid (通り芯) parsing (src/parse/Grid): IfcGridAxis -> polyline endpoints, dedup,
# bbox centering, X/Y classification (by name, else by orientation) and class
# assignment. Exercised on synthetic STEP snippets and the checked-in fixtures;
# SDK-free. HOMESKZ_FIXTURES_DIR points the file-based cases at tests/fixtures/.
vw_add_test(ParseGridTests ParseGridTests.cpp)
target_link_libraries(ParseGridTests PRIVATE HomeskzIfcCore)
target_compile_definitions(ParseGridTests
						   PRIVATE "HOMESKZ_FIXTURES_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\"")

# Geometry foundation (src/core/Geometry + src/parse/IfcGeometry): placement-matrix
# composition (IfcLocalPlacement / IfcAxis2Placement3D/2D), profile outlines
# (IfcRectangleProfileDef / IfcArbitraryClosedProfileDef), extruded-area-solid world
# transform (vertical & horizontal extrusion) and IfcBooleanResult first-operand walk.
# Exercised on hand-computed synthetic snippets and a real fixture; SDK-free.
# HOMESKZ_FIXTURES_DIR points the file-based cases at tests/fixtures/.
vw_add_test(GeometryTests GeometryTests.cpp)
target_link_libraries(GeometryTests PRIVATE HomeskzIfcCore)
target_compile_definitions(GeometryTests
						   PRIVATE "HOMESKZ_FIXTURES_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\"")

# Story (ストーリ) parsing (src/parse/Story) + the SDK-free desired-layer-order
# computation (src/core/desiredStoryLayerOrder): IfcBuildingStorey -> stories sorted by
# Elevation, beam-top offset from contained IfcColumn/IfcSlab local Z, base level/layer
# assembly (FL + 横架材天端, top = 軒高) and the layer stack order (共通 first, top→bottom,
# floor to background). Exercised on synthetic STEP snippets and a real fixture; SDK-free.
# HOMESKZ_FIXTURES_DIR points the file-based cases at tests/fixtures/.
vw_add_test(ParseStoryTests ParseStoryTests.cpp)
target_link_libraries(ParseStoryTests PRIVATE HomeskzIfcCore)
target_compile_definitions(ParseStoryTests
						   PRIVATE "HOMESKZ_FIXTURES_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\"")

# Structural-class assignment (src/parse/StructuralClass): the pure member-type ->
# VW class logic (04構造-02木造-… leaf classes). Trusts the homeskz IFC Name token when
# it identifies a type, else infers from story/height (member: dodai/yukabari/koyabari/
# moya; column: koyazuka/toshibashira/kudabashira). No STEP graph, no fixtures — pure
# string/index logic, so it links the library but needs no HOMESKZ_FIXTURES_DIR.
vw_add_test(ParseStructuralClassTests ParseStructuralClassTests.cpp)
target_link_libraries(ParseStructuralClassTests PRIVATE HomeskzIfcCore)

# Floor (床板) parsing (src/parse/Floor): IfcSlab "床版" -> per-storey FL-layer floor
# commands with a fixed 24mm thickness, the footprint centred on the grid centre, the
# floor's bottom Z taken straight from the IFC (so skip-floor steps survive) and the
# height bound to the 横架材天端 level with that difference as its offset. Exercised on
# synthetic STEP snippets and the checked-in fixtures; SDK-free. HOMESKZ_FIXTURES_DIR
# points the file-based cases at tests/fixtures/.
vw_add_test(ParseFloorTests ParseFloorTests.cpp)
target_link_libraries(ParseFloorTests PRIVATE HomeskzIfcCore)
target_compile_definitions(ParseFloorTests
						   PRIVATE "HOMESKZ_FIXTURES_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\"")

# Floor VectorScript assembly (src/core/FloorScript): the SDK has no API to create a
# Floor object, so draw/Floor runs a VectorScript through the SDK's script engine. The
# script TEXT (command order, coordinate formatting, quoting) is pure string logic, so it
# is built in core/ and verified here without the SDK — mirroring vw/floor.py's draw_floor.
vw_add_test(CoreFloorScriptTests CoreFloorScriptTests.cpp)
target_link_libraries(CoreFloorScriptTests PRIVATE HomeskzIfcCore)

# IFC read-result summary (src/parse/Summary): counts the homeskz-relevant entity
# types and formats them into the dialog text the import command shows. SDK-free —
# the counting + formatting is tested here; only the file picker and the alert
# itself live in the SDK menu command. HOMESKZ_FIXTURES_DIR points the file-based
# cases at the checked-in fixtures (tests/fixtures/).
vw_add_test(ParseSummaryTests ParseSummaryTests.cpp)
target_link_libraries(ParseSummaryTests PRIVATE HomeskzIfcCore)
target_compile_definitions(ParseSummaryTests
						   PRIVATE "HOMESKZ_FIXTURES_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\"")

# Back-end tests for the bundled updater scripts. Both SOURCE (dot-source) their
# script and drive its q-stable / q-dev / do-install functions with the network
# boundary stubbed, so they run SDK- and network-free — the same idea as the C++
# tests, and cross-platform: PowerShell (pwsh) runs on the Linux CI runner too.
# Each is registered only when its interpreter is found, and each harness itself
# skips (exit 0) when a tool it needs is missing.

# VW_REQUIRE_SCRIPT_TESTS is threaded into each harness' environment so a missing
# helper tool (python3/unzip/zip inside the bash suite) fails instead of skipping.
# The value is passed through verbatim; the harnesses treat falsy spellings as
# unset, so an OFF build still skips gracefully.

# macOS updater (scripts/vw-update.sh) via bash + stubbed curl/plutil.
if(NOT WIN32)
	find_program(BASH_PROGRAM bash)
	if(BASH_PROGRAM)
		add_test(NAME UpdaterScriptTests COMMAND "${BASH_PROGRAM}"
												 "${CMAKE_CURRENT_SOURCE_DIR}/vw-update.test.sh")
		set_tests_properties(
			UpdaterScriptTests PROPERTIES ENVIRONMENT
										  "VW_REQUIRE_SCRIPT_TESTS=${VW_REQUIRE_SCRIPT_TESTS}")
	elseif(VW_REQUIRE_SCRIPT_TESTS)
		message(FATAL_ERROR "VW_REQUIRE_SCRIPT_TESTS is ON but 'bash' was not found: the macOS "
							"updater-script tests (vw-update.test.sh) cannot run.")
	endif()
endif()

# Windows updater (scripts/vw-update.ps1) via PowerShell 7 (pwsh) + stubbed
# Invoke-GH/Invoke-WebRequest. No Pester needed — the harness is self-contained.
find_program(PWSH_PROGRAM pwsh)
if(PWSH_PROGRAM)
	add_test(NAME UpdaterScriptTestsPs COMMAND "${PWSH_PROGRAM}" -NoProfile -File
											   "${CMAKE_CURRENT_SOURCE_DIR}/vw-update.Tests.ps1")
	set_tests_properties(
		UpdaterScriptTestsPs PROPERTIES ENVIRONMENT
										"VW_REQUIRE_SCRIPT_TESTS=${VW_REQUIRE_SCRIPT_TESTS}")
elseif(VW_REQUIRE_SCRIPT_TESTS)
	message(FATAL_ERROR "VW_REQUIRE_SCRIPT_TESTS is ON but 'pwsh' (PowerShell 7) was not found: "
						"the Windows updater-script tests (vw-update.Tests.ps1) cannot run.")
endif()
