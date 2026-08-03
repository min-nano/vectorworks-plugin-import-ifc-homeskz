# Unit tests for the plug-in's SDK-independent code. Two families of executables:
#
#   * the updater tests (UpdaterParse / UpdaterFlow / UpdaterRobustness + the two
#     bundled-script suites) — the template's own SDK-free logic;
#   * the 2-phase tests (Core* / Step / Loader / Parse*) — the IFC import's
#     SDK-free half (src/core/ + src/parse/), each linking the HomeskzIfcCore
#     library. A new parse module gets a new vw_add_test entry here.
#
# None of them links the Vectorworks SDK, so they build and run on any toolchain —
# including a plain Linux runner — which is what makes the coverage job cheap.
# (The SDK-dependent draw/ phase has no unit tests by design: its behaviour is
# verified on a real Vectorworks install. See tests/README.md.)
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

# The instruction set (src/core/Document): the command structs' invariants and
# validateDocument's gate for every command list that has landed so far (stories,
# grids, floors, members, columns, rafters, roofs), plus the desired layer-stack order.
# Links the HomeskzIfcCore library (defined in the top-level CMakeLists.txt) rather than
# compiling those sources directly, which also proves the CMake target split
# builds and links with no SDK present. Extend it whenever a command list is
# added to core/Document.h.
vw_add_test(CoreDocumentTests CoreDocumentTests.cpp)
target_link_libraries(CoreDocumentTests PRIVATE HomeskzIfcCore)

# Progress reporting (src/core/Progress): the display-text formatting, the
# proportional progress-bar share per phase, the step counting (reset per phase,
# clamped to the total) and the latched cancellation flag. Pure logic shared by
# both phases — the actual dialog (draw/ProgressDialog) is SDK-side and verified
# on a real Vectorworks install. SDK-free, no fixtures.
vw_add_test(CoreProgressTests CoreProgressTests.cpp)
target_link_libraries(CoreProgressTests PRIVATE HomeskzIfcCore)

# Minimal STEP reader (src/parse/Step): tokeniser + entity graph exercised on
# small synthetic STEP snippets (references, numbers, strings, enums, nested
# lists, typed values, referrers/resolve, determinism). SDK-free.
vw_add_test(StepTests StepTests.cpp)
target_link_libraries(StepTests PRIVATE HomeskzIfcCore)

# IFC loader (src/parse/Loader): proves the loader does NOT sanitise — the in-house
# STEP reader has no schema, so non-standard entities (the IFC4-only IFCFOOTINGTYPE
# that homeskz mixes into IFC2X3) are read as-is rather than stripped the way the
# Python version had to (see src/parse/Loader.h for why) — plus real-fixture file
# loading. HOMESKZ_FIXTURES_DIR points the file-based cases at the checked-in
# fixtures (tests/fixtures/), reused as parse modules land.
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

# Planar region synthesis (src/core/Region): the outline of the area a set of parts
# encloses (enclosed voids plus the parts bounding them), used to synthesise the loft
# floor from the roof storey's floor beams when homeskz emits no floor slab for it.
# Pure geometry — no STEP, no SDK, no fixtures.
vw_add_test(CoreRegionTests CoreRegionTests.cpp)
target_link_libraries(CoreRegionTests PRIVATE HomeskzIfcCore)

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
# assembly (FL + 横架材天端, top = 軒高), the span column layer name round-trip
# ({from}to{to}-柱) with its per-storey levels, and the layer stack order (共通 first,
# top→bottom, floor to background). Exercised on synthetic STEP snippets and a real fixture; SDK-free.
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

# Floor (床板) parsing (src/parse/Floor): IfcSlab "床版" -> per-storey FL-layer slab
# commands — the per-storey slab style and its 床仕上げ/床下地 components, the datum
# face (Top on a normal storey, Bottom for the loft), the footprint centred on the
# grid centre, the floor's Z taken straight from the IFC (so skip-floor steps survive)
# bound to the FL level (軒高 for the loft) with that difference as its offset, and the
# loft floor synthesised from the roof storey's floor beams when no floor slab exists.
# Exercised on synthetic STEP snippets and the checked-in fixtures; SDK-free.
# HOMESKZ_FIXTURES_DIR points the file-based cases at tests/fixtures/.
vw_add_test(ParseFloorTests ParseFloorTests.cpp)
target_link_libraries(ParseFloorTests PRIVATE HomeskzIfcCore)
target_compile_definitions(ParseFloorTests
						   PRIVATE "HOMESKZ_FIXTURES_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\"")

# Rafter (垂木) parsing (src/parse/Rafter): roof slabs (IfcSlab "屋根版") -> per-storey
# 垂木-layer rafter commands derived from the roof face — sweep spacing (ends inset by half
# the rafter width, interior <= 455mm), scan-line clipping of non-convex faces, the support
# point where the roof face crosses the 横架材天端 level, overhang/embedment and the spec
# label. Exercised on synthetic planes/STEP snippets and the checked-in fixtures; SDK-free.
# HOMESKZ_FIXTURES_DIR points the file-based cases at tests/fixtures/.
vw_add_test(ParseRafterTests ParseRafterTests.cpp)
target_link_libraries(ParseRafterTests PRIVATE HomeskzIfcCore)
target_compile_definitions(ParseRafterTests
						   PRIVATE "HOMESKZ_FIXTURES_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\"")

# Roof sheathing (野地板) parsing (src/parse/Roof): one roof command per roof-slab face —
# eaves axis on the lowest edge, upslope toward the ridge, slope as rise/run from the face
# normal, the 12mm fixed thickness and the eaves Z lifted by the rafter depth. Exercised on
# synthetic planes/STEP snippets and the checked-in fixtures; SDK-free.
# HOMESKZ_FIXTURES_DIR points the file-based cases at tests/fixtures/.
vw_add_test(ParseRoofTests ParseRoofTests.cpp)
target_link_libraries(ParseRoofTests PRIVATE HomeskzIfcCore)
target_compile_definitions(ParseRoofTests
						   PRIVATE "HOMESKZ_FIXTURES_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\"")

# Member (横架材) parsing (src/parse/Member): IfcBeam / IfcMember -> structural-member
# commands — the section-centre-to-top-centre reference correction, per-beam elevation from
# the element's own local Z, story-level binding (横架材天端 / 軒高 / 母屋 / 登り梁), the
# sloped-beam (登り梁) arbitrary-profile extraction with its square-cut geometry, and the
# T/L interference trimming. Exercised on synthetic STEP snippets and the checked-in
# fixtures; SDK-free. HOMESKZ_FIXTURES_DIR points the file-based cases at tests/fixtures/.
vw_add_test(ParseMemberTests ParseMemberTests.cpp)
target_link_libraries(ParseMemberTests PRIVATE HomeskzIfcCore)
target_compile_definitions(ParseMemberTests
						   PRIVATE "HOMESKZ_FIXTURES_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\"")

# Column (柱) parsing (src/parse/Column): IfcColumn -> vertical structural-member commands —
# the span layer ({from}to{to}-柱) with its to-level rule (next floor for a 管柱, +0.5 for a roof
# post, several floors up for a 通し柱, all measured against the upper storeys' beam BOTTOMS),
# the story-level binding of both ends (柱 = current + upper storey, 小屋束 = current twice with
# equal offsets so the path length is not double-counted), the structural use, the column-head /
# base hardware matched by plan position and folded into the member ID, and the 小屋束 section
# resized to the width of the 母屋/棟木/登り梁 resting on it. Exercised on synthetic STEP snippets
# and the checked-in fixtures; SDK-free.
# HOMESKZ_FIXTURES_DIR points the file-based cases at tests/fixtures/.
vw_add_test(ParseColumnTests ParseColumnTests.cpp)
target_link_libraries(ParseColumnTests PRIVATE HomeskzIfcCore)
target_compile_definitions(ParseColumnTests
						   PRIVATE "HOMESKZ_FIXTURES_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\"")

# Foundation (基礎) parsing (src/parse/Footing): IfcFooting "基礎梁…" -> wall commands and
# IfcSlab/IfcFooting "…底盤…" -> slab commands, plus the 基礎 storey (suffix F, GL=0, the
# GL/底盤天端 levels and their F-立上り/F-底盤 layers). Covers the slab-top elevation (largest
# summed footprint area wins), the wall bottom taken straight from the IFC solid, the wall merge
# (collinear + same section only) and free-end half-thickness extension snapped to the terminal
# column centre, the slab merge (polygon union over edge-sharing components; gaps, corner
# touches and rings with holes are left alone) and the outer-face alignment (each edge pushed
# out by the half thickness of the wall it runs along). Exercised on synthetic commands and the
# checked-in fixtures; SDK-free.
# HOMESKZ_FIXTURES_DIR points the file-based cases at tests/fixtures/.
vw_add_test(ParseFootingTests ParseFootingTests.cpp)
target_link_libraries(ParseFootingTests PRIVATE HomeskzIfcCore)
target_compile_definitions(ParseFootingTests
						   PRIVATE "HOMESKZ_FIXTURES_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\"")

# Sloped-beam (登り梁) correction (src/parse/Noboribari): trims the ends that homeskz sinks
# into the receiving members and snaps the top centre line onto the roof face fixed in M6,
# so the pitch and height match the rafter underside. Exercised on synthetic planes/commands
# and the checked-in fixtures; SDK-free.
# HOMESKZ_FIXTURES_DIR points the file-based cases at tests/fixtures/.
vw_add_test(ParseNoboribariTests ParseNoboribariTests.cpp)
target_link_libraries(ParseNoboribariTests PRIVATE HomeskzIfcCore)
target_compile_definitions(ParseNoboribariTests
						   PRIVATE "HOMESKZ_FIXTURES_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\"")

# Anchor-bolt (アンカーボルト) parsing (src/parse/AnchorBolt): IfcMechanicalFastener bolt
# BODIES (never the washers, which share the axis and would double-count) -> symbol commands
# on the foundation's F-アンカーボルト layer, with the M12 / M16 split driven by whether the
# fastener type name says 座金なし. Exercised on synthetic STEP snippets and the checked-in
# fixtures; SDK-free. HOMESKZ_FIXTURES_DIR points the file-based cases at tests/fixtures/.
vw_add_test(ParseAnchorBoltTests ParseAnchorBoltTests.cpp)
target_link_libraries(ParseAnchorBoltTests PRIVATE HomeskzIfcCore)
target_compile_definitions(ParseAnchorBoltTests
						   PRIVATE "HOMESKZ_FIXTURES_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\"")

# Floor-post (床束) parsing (src/parse/FloorPost): homeskz emits no floor posts, so they are
# placed at a fixed 910mm pitch under the 大引 — after merging the runs split by 継手 (a gap of
# at most half a module) and after moving each end from the real member end onto the CENTRE of
# the 土台 / 大引 that supports it. Exercised on synthetic geometry/STEP snippets and the
# checked-in fixtures; SDK-free.
# HOMESKZ_FIXTURES_DIR points the file-based cases at tests/fixtures/.
vw_add_test(ParseFloorPostTests ParseFloorPostTests.cpp)
target_link_libraries(ParseFloorPostTests PRIVATE HomeskzIfcCore)
target_compile_definitions(ParseFloorPostTests
						   PRIVATE "HOMESKZ_FIXTURES_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\"")

# Fire-brace (火打) parsing (src/parse/FireBrace): vertically extruded IfcBeam / IfcMember named
# 火打… -> 鋼製火打 symbols on the beam layer — the two end faces picked out of the footprint by
# the sign flip of the profile's local v, their extended intersection as the inside-corner base
# point, and the angle bisector plus the symbol's 45-degree orientation offset. Exercised on
# synthetic footprints/STEP snippets and the checked-in fixtures; SDK-free.
# HOMESKZ_FIXTURES_DIR points the file-based cases at tests/fixtures/.
vw_add_test(ParseFireBraceTests ParseFireBraceTests.cpp)
target_link_libraries(ParseFireBraceTests PRIVATE HomeskzIfcCore)
target_compile_definitions(ParseFireBraceTests
						   PRIVATE "HOMESKZ_FIXTURES_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\"")

# Joint (仕口) parsing (src/parse/Joint): the only parse module that never touches IFC — it reads
# the finished member/column commands and marks the beam ends that actually land on a receiving
# member or column (parallel splices, other layers and separated Z ranges excluded; 登り梁 alone
# may reach across layers). Exercised on synthetic commands and the checked-in fixtures; SDK-free.
# HOMESKZ_FIXTURES_DIR points the file-based cases at tests/fixtures/.
vw_add_test(ParseJointTests ParseJointTests.cpp)
target_link_libraries(ParseJointTests PRIVATE HomeskzIfcCore)
target_compile_definitions(ParseJointTests
						   PRIVATE "HOMESKZ_FIXTURES_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\"")

# Shared parse context (src/parse/Context): the per-import cache that lets every
# element's build*Commands reuse the same stories / grid centre / storey elements /
# loft-floor regions / roof planes instead of recomputing them. Proves the cached
# answers are stable across repeated calls AND identical to the uncached functions —
# the cache must not change behaviour. Exercised on the checked-in fixtures; SDK-free.
# HOMESKZ_FIXTURES_DIR points the file-based cases at tests/fixtures/.
vw_add_test(ParseContextTests ParseContextTests.cpp)
target_link_libraries(ParseContextTests PRIVATE HomeskzIfcCore)
target_compile_definitions(ParseContextTests
						   PRIVATE "HOMESKZ_FIXTURES_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\"")

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

# CI-wait clients (scripts/ci-common.sh + scripts/ci-wait.sh) via bash + a fake
# `curl` on PATH. These are developer tooling rather than plug-in code, but the
# property they encode — a wait that ALWAYS terminates and never mistakes "CI has
# not started" or "run cancelled by a newer push" for a green build — has broken
# twice, and each break cost a session that never noticed CI had finished. Same
# skip/require policy as the updater harnesses above.
if(NOT WIN32)
	find_program(BASH_PROGRAM bash)
	if(BASH_PROGRAM)
		add_test(NAME CiWaitScriptTests COMMAND "${BASH_PROGRAM}"
												"${CMAKE_CURRENT_SOURCE_DIR}/ci-wait.test.sh")
		set_tests_properties(
			CiWaitScriptTests PROPERTIES ENVIRONMENT
										 "VW_REQUIRE_SCRIPT_TESTS=${VW_REQUIRE_SCRIPT_TESTS}")
		# The suite deliberately waits on real deadlines (a few seconds each), and
		# has its own ceiling; keep ctest's above it so a hang is reported by the
		# suite itself rather than as an opaque ctest timeout.
		set_tests_properties(CiWaitScriptTests PROPERTIES TIMEOUT 900)
	elseif(VW_REQUIRE_SCRIPT_TESTS)
		message(FATAL_ERROR "VW_REQUIRE_SCRIPT_TESTS is ON but 'bash' was not found: the CI-wait "
							"script tests (ci-wait.test.sh) cannot run.")
	endif()
endif()
