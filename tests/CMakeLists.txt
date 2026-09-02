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

# Registers one test executable. Options after the sources:
#   * CORE     … links the HomeskzIfcCore library (the SDK-free src/core/ + src/parse/
#                half). Every 2-phase test wants this; the updater tests do not.
#   * FIXTURES … defines HOMESKZ_FIXTURES_DIR so the file-based cases find the
#                checked-in fixtures (tests/fixtures/). Deliberately opt-in per
#                target: tests/Fixtures.h compiles fixture() only under
#                #ifdef HOMESKZ_FIXTURES_DIR, so a test that uses fixtures without
#                declaring FIXTURES fails to compile instead of silently passing.
function(vw_add_test name)
	cmake_parse_arguments(
		ARG
		"CORE;FIXTURES"
		""
		""
		${ARGN})
	add_executable(${name} ${ARG_UNPARSED_ARGUMENTS})
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
	if(ARG_CORE)
		target_link_libraries(${name} PRIVATE HomeskzIfcCore)
	endif()
	if(ARG_FIXTURES)
		target_compile_definitions(
			${name} PRIVATE "HOMESKZ_FIXTURES_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\"")
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
vw_add_test(CoreDocumentTests CoreDocumentTests.cpp CORE)

# The foundation as one object (src/core/Foundation): the parametric ground-beam
# section (fit / rebuild round trip), the representative values and how an edit is
# spread over the parts, the solids the PIO draws (slab, gravel, risers, beams with
# the bite, the bedding under the ground beams incl. the perimeter spill and the
# clipping against crossing beams), the plan outlines, and the record text the PIO
# keeps its parts in (encode / decode). Uses the real fixtures for the bedding
# invariants that M17 established.
vw_add_test(CoreFoundationTests CoreFoundationTests.cpp CORE FIXTURES)

# Progress reporting (src/core/Progress): the display-text formatting, the
# proportional progress-bar share per phase, the step counting (reset per phase,
# clamped to the total) and the latched cancellation flag. Pure logic shared by
# both phases — the actual dialog (draw/ProgressDialog) is SDK-side and verified
# on a real Vectorworks install. SDK-free, no fixtures.
vw_add_test(CoreProgressTests CoreProgressTests.cpp CORE)

# Sheet-layer layout (src/core/Layout): the scale ladder and how a scale is picked
# to fit the paper, the plan sheets' shared scale/centre with the legend column kept
# clear, and the section sheets' two-row grid (slot centres, sheets needed, titles).
# Pure arithmetic shared by both phases — the actual paper size comes from the SDK
# (draw/DrawUtil's SheetPaperArea) and is verified on a real Vectorworks install.
# SDK-free, no fixtures.
vw_add_test(CoreLayoutTests CoreLayoutTests.cpp CORE)

# Crash-diagnosis log (src/core/Trace): enable/disable, per-line flushing (a crash
# must not eat the last line), truncate-on-reopen, a non-fatal open failure, the
# default temp-dir path, and the one place phases reach the log (core/Progress's
# beginPhase). SDK-free; writes into the build tree and removes what it wrote.
vw_add_test(CoreTraceTests CoreTraceTests.cpp CORE)
# Where the cases write their log files. Deliberately NOT the temp dir the plug-in
# itself uses: deriving the path from TMPDIR/TEMP inside the test would make CodeQL
# report an environment-variable-derived path reaching a file open (cpp/path-injection),
# and writing into the build tree keeps the shared temp dir clean.
target_compile_definitions(CoreTraceTests
						   PRIVATE "HOMESKZ_TRACE_TEST_DIR=\"${CMAKE_CURRENT_BINARY_DIR}\"")

# Minimal STEP reader (src/parse/Step): tokeniser + entity graph exercised on
# small synthetic STEP snippets (references, numbers, strings, enums, nested
# lists, typed values, referrers/resolve, determinism). SDK-free.
vw_add_test(StepTests StepTests.cpp CORE)

# IFC loader (src/parse/Loader): proves the loader does NOT sanitise — the in-house
# STEP reader has no schema, so non-standard entities (the IFC4-only IFCFOOTINGTYPE
# that homeskz mixes into IFC2X3) are read as-is rather than stripped the way the
# Python version had to (see src/parse/Loader.h for why) — plus real-fixture file
# loading. HOMESKZ_FIXTURES_DIR points the file-based cases at the checked-in
# fixtures (tests/fixtures/), reused as parse modules land.
vw_add_test(LoaderTests LoaderTests.cpp CORE FIXTURES)

# Grid (通り芯) parsing (src/parse/Grid): IfcGridAxis -> polyline endpoints, dedup,
# bbox centering, X/Y classification (by name, else by orientation) and class
# assignment. Exercised on synthetic STEP snippets and the checked-in fixtures;
# SDK-free. HOMESKZ_FIXTURES_DIR points the file-based cases at tests/fixtures/.
vw_add_test(ParseGridTests ParseGridTests.cpp CORE FIXTURES)

# Planar region synthesis (src/core/Region): the outline of the area a set of parts
# encloses (enclosed voids plus the parts bounding them), used to synthesise the loft
# floor from the roof storey's floor beams when homeskz emits no floor slab for it.
# Pure geometry — no STEP, no SDK, no fixtures.
vw_add_test(CoreRegionTests CoreRegionTests.cpp CORE)

# Polygon set operations (src/core/PolygonBool): the union that folds same-spec
# foundation outlines together, the difference that keeps the slab's gravel out of the
# ground-beam concrete, the "connected" predicate (shared edge / overlap, but not a bare
# corner) and the component grouping. Pins down that a ring of bands keeps its hole, so
# a lattice of strip footings is never folded into one filled slab. Pure geometry — no
# STEP, no SDK, no fixtures.
vw_add_test(CorePolygonBoolTests CorePolygonBoolTests.cpp CORE)

# Connected components (src/core/UnionFind): the pairwise-predicate union-find that the
# wall / ohbiki / ground-beam merges and the wall-join clustering all share. Pins down
# the determinism contract (representative = smallest index, components in
# representative order, ascending inside). Pure logic — no STEP, no SDK, no fixtures.
vw_add_test(CoreUnionFindTests CoreUnionFindTests.cpp CORE)

# Geometry foundation (src/core/Geometry + src/parse/IfcGeometry): placement-matrix
# composition (IfcLocalPlacement / IfcAxis2Placement3D/2D), profile outlines
# (IfcRectangleProfileDef / IfcArbitraryClosedProfileDef), extruded-area-solid world
# transform (vertical & horizontal extrusion) and IfcBooleanResult first-operand walk.
# Exercised on hand-computed synthetic snippets and a real fixture; SDK-free.
# HOMESKZ_FIXTURES_DIR points the file-based cases at tests/fixtures/.
vw_add_test(GeometryTests GeometryTests.cpp CORE FIXTURES)

# Story (ストーリ) parsing (src/parse/Story) + the SDK-free desired-layer-order
# computation (src/core/desiredStoryLayerOrder): IfcBuildingStorey -> stories sorted by
# Elevation, beam-top offset from contained IfcColumn/IfcSlab local Z, base level/layer
# assembly (FL + 横架材天端, top = 軒高), the span column layer name round-trip
# ({from}to{to}-柱) with its per-storey levels, and the layer stack order (共通 first,
# top→bottom, floor to background). Exercised on synthetic STEP snippets and a real fixture; SDK-free.
# HOMESKZ_FIXTURES_DIR points the file-based cases at tests/fixtures/.
vw_add_test(ParseStoryTests ParseStoryTests.cpp CORE FIXTURES)

# Sheet (伏図) parsing (src/parse/Sheet): which plan sheets to make and what each viewport
# shows — the foundation plan (only when foundation elements exist), one framing plan per
# storey and one moya plan per storey with a roof slab, the sheet numbering (1 → 2… → moya),
# the titles (床/小屋/母屋) and the cut-level filter that picks the span column layers
# ({from}to{to}-柱) a plan crosses. Also cross-checks that every layer a viewport lists is
# one the story commands actually create. Exercised on the checked-in fixtures; SDK-free.
# HOMESKZ_FIXTURES_DIR points the file-based cases at tests/fixtures/.
vw_add_test(ParseSheetTests ParseSheetTests.cpp CORE FIXTURES)

# Section (軸組図) parsing (src/parse/Section): which grid lines get a section viewport and
# what each one cuts — the cut positions where both a column and a parallel beam run (大引 and
# 母屋 do not count as beams), the naming against the IFC grid axes (an exact axis name, else
# an intermediate line as X1' / 又い), the section line geometry (constant X / constant Y plus
# a margin), the view direction (so grid names read left to right), the height range that wraps
# the model and the design layers a section shows. Synthetic STEP grid axes plus one real
# fixture end to end; SDK-free. HOMESKZ_FIXTURES_DIR points the file-based case at fixtures/.
vw_add_test(ParseSectionTests ParseSectionTests.cpp CORE FIXTURES)

# Data tags (src/parse/Tag): the 断面寸法 data tags that annotate each 横架材 on a
# viewport — the readable text angle ((-90, 90] so it never reads upside down), the side the
# tag is nudged to (up for east-west members, left for north-south ones), which members a plan
# viewport tags (those whose layer it shows) and where the tag sits (mid of the member's edge,
# flush so VW draws no leader), and — new against the Python version — which members a section
# viewport tags (only those lying on the cut plane) and how they project into the section's
# annotation space (distance along the cut line, elevation). Runs end to end on the checked-in
# fixtures; SDK-free. HOMESKZ_FIXTURES_DIR points the file-based cases at tests/fixtures/.
vw_add_test(ParseTagTests ParseTagTests.cpp CORE FIXTURES)

# Structural-class assignment (src/parse/StructuralClass): the pure member-type ->
# VW class logic (04構造-02木造-… leaf classes). Trusts the homeskz IFC Name token when
# it identifies a type, else infers from story/height (member: dodai/yukabari/koyabari/
# moya; column: koyazuka/toshibashira/kudabashira). No STEP graph, no fixtures — pure
# string/index logic, so it links the library but needs no HOMESKZ_FIXTURES_DIR.
vw_add_test(ParseStructuralClassTests ParseStructuralClassTests.cpp CORE)

# Floor (床板) parsing (src/parse/Floor): IfcSlab "床版" -> per-storey FL-layer slab
# commands — the per-storey slab style and its 床仕上げ/床下地 components, the datum
# face (Top on a normal storey, Bottom for the loft), the footprint centred on the
# grid centre, the floor's Z taken straight from the IFC (so skip-floor steps survive)
# bound to the FL level (軒高 for the loft) with that difference as its offset, and the
# loft floor synthesised from the roof storey's floor beams when no floor slab exists.
# Exercised on synthetic STEP snippets and the checked-in fixtures; SDK-free.
# HOMESKZ_FIXTURES_DIR points the file-based cases at tests/fixtures/.
vw_add_test(ParseFloorTests ParseFloorTests.cpp CORE FIXTURES)

# Rafter (垂木) parsing (src/parse/Rafter): roof slabs (IfcSlab "屋根版") -> per-storey
# 垂木-layer rafter commands derived from the roof face — sweep spacing (ends inset by half
# the rafter width, interior <= 455mm), scan-line clipping of non-convex faces, the support
# point where the roof face crosses the 横架材天端 level, overhang/embedment and the spec
# label. Exercised on synthetic planes/STEP snippets and the checked-in fixtures; SDK-free.
# HOMESKZ_FIXTURES_DIR points the file-based cases at tests/fixtures/.
vw_add_test(ParseRafterTests ParseRafterTests.cpp CORE FIXTURES)

# Roof sheathing (野地板) parsing (src/parse/Roof): one roof command per roof-slab face —
# eaves axis on the lowest edge, upslope toward the ridge, slope as rise/run from the face
# normal, the 12mm fixed thickness and the eaves Z lifted by the rafter depth. Exercised on
# synthetic planes/STEP snippets and the checked-in fixtures; SDK-free.
# HOMESKZ_FIXTURES_DIR points the file-based cases at tests/fixtures/.
vw_add_test(ParseRoofTests ParseRoofTests.cpp CORE FIXTURES)

# Member (横架材) parsing (src/parse/Member): IfcBeam / IfcMember -> structural-member
# commands — the section-centre-to-top-centre reference correction, per-beam elevation from
# the element's own local Z, story-level binding (横架材天端 / 軒高 / 母屋 / 登り梁), the
# sloped-beam (登り梁) arbitrary-profile extraction with its square-cut geometry, and the
# T/L interference trimming. Exercised on synthetic STEP snippets and the checked-in
# fixtures; SDK-free. HOMESKZ_FIXTURES_DIR points the file-based cases at tests/fixtures/.
vw_add_test(ParseMemberTests ParseMemberTests.cpp CORE FIXTURES)

# Column (柱) parsing (src/parse/Column): IfcColumn -> vertical structural-member commands —
# the span layer ({from}to{to}-柱) with its to-level rule (next floor for a 管柱, +0.5 for a roof
# post, several floors up for a 通し柱, all measured against the upper storeys' beam BOTTOMS),
# the story-level binding of both ends (柱 = current + upper storey, 小屋束 = current twice with
# equal offsets so the path length is not double-counted), the structural use, the column-head /
# base hardware matched by plan position and folded into the member ID, and the 小屋束 section
# resized to the width of the 母屋/棟木/登り梁 resting on it. Exercised on synthetic STEP snippets
# and the checked-in fixtures; SDK-free.
# HOMESKZ_FIXTURES_DIR points the file-based cases at tests/fixtures/.
vw_add_test(ParseColumnTests ParseColumnTests.cpp CORE FIXTURES)

# Foundation (基礎) parsing (src/parse/Footing): IfcFooting "基礎梁…" -> wall commands and
# IfcSlab/IfcFooting "…底盤…" -> slab commands, plus the 基礎 storey (suffix F, GL=0, the
# GL/底盤天端 levels and their F-立上り/F-底盤 layers). Covers the slab-top elevation (largest
# summed footprint area wins), the wall bottom taken straight from the IFC solid, the wall merge
# (collinear + same section only) and free-end half-thickness extension snapped to the terminal
# column centre, the slab merge (polygon union over edge-sharing components; gaps, corner
# touches and rings with holes are left alone) and the outer-face alignment (each edge pushed
# out by the half thickness of the wall it runs along). Also covers the M10 additions: person
# passages (人通口) carved out of the merged walls (split when the opening bottom sits at or below
# the slab top, top lowered to the opening bottom otherwise), the wall joins (L/T/X per junction,
# capped when the tops differ, pick points nudged to the kept side) and the ground beams (地中梁)
# turned into trapezoid-prism modifiers, merged along a shared axis and handed to the slab they
# overlap. Exercised on synthetic commands and the checked-in fixtures; SDK-free.
# HOMESKZ_FIXTURES_DIR points the file-based cases at tests/fixtures/.
vw_add_test(ParseFootingTests ParseFootingTests.cpp CORE FIXTURES)

# Sloped-beam (登り梁) correction (src/parse/Noboribari): trims the ends that homeskz sinks
# into the receiving members and snaps the top centre line onto the roof face fixed in M6,
# so the pitch and height match the rafter underside. Exercised on synthetic planes/commands
# and the checked-in fixtures; SDK-free.
# HOMESKZ_FIXTURES_DIR points the file-based cases at tests/fixtures/.
vw_add_test(ParseNoboribariTests ParseNoboribariTests.cpp CORE FIXTURES)

# Anchor-bolt (アンカーボルト) parsing (src/parse/AnchorBolt): IfcMechanicalFastener bolt
# BODIES (never the washers, which share the axis and would double-count) -> symbol commands
# on the foundation's F-アンカーボルト layer, with the M12 / M16 split driven by whether the
# fastener type name says 座金なし. Exercised on synthetic STEP snippets and the checked-in
# fixtures; SDK-free. HOMESKZ_FIXTURES_DIR points the file-based cases at tests/fixtures/.
vw_add_test(ParseAnchorBoltTests ParseAnchorBoltTests.cpp CORE FIXTURES)

# Floor-post (床束) parsing (src/parse/FloorPost): homeskz emits no floor posts, so they are
# placed at a fixed 910mm pitch under the 大引 — after merging the runs split by 継手 (a gap of
# at most half a module) and after moving each end from the real member end onto the CENTRE of
# the 土台 / 大引 that supports it. Exercised on synthetic geometry/STEP snippets and the
# checked-in fixtures; SDK-free.
# HOMESKZ_FIXTURES_DIR points the file-based cases at tests/fixtures/.
vw_add_test(ParseFloorPostTests ParseFloorPostTests.cpp CORE FIXTURES)

# Fire-brace (火打) parsing (src/parse/FireBrace): vertically extruded IfcBeam / IfcMember named
# 火打… -> 鋼製火打 symbols on the beam layer — the two end faces picked out of the footprint by
# the sign flip of the profile's local v, their extended intersection as the inside-corner base
# point, and the angle bisector plus the symbol's 45-degree orientation offset. Exercised on
# synthetic footprints/STEP snippets and the checked-in fixtures; SDK-free.
# HOMESKZ_FIXTURES_DIR points the file-based cases at tests/fixtures/.
vw_add_test(ParseFireBraceTests ParseFireBraceTests.cpp CORE FIXTURES)

# Joint (仕口) parsing (src/parse/Joint): the only parse module that never touches IFC — it reads
# the finished member/column commands and marks the beam ends that actually land on a receiving
# member or column (parallel splices, other layers and separated Z ranges excluded; 登り梁 alone
# may reach across layers). Exercised on synthetic commands and the checked-in fixtures; SDK-free.
# HOMESKZ_FIXTURES_DIR points the file-based cases at tests/fixtures/.
vw_add_test(ParseJointTests ParseJointTests.cpp CORE FIXTURES)

# Column marks (src/parse/ColumnMark): the section marks (× / ／ drawn as plain lines on the
# column's own span layer) and the plan marks (VW data tags on the independent "{to}-柱伏図記号"
# layer). Like the joints, this module never touches IFC — the marks are derived from the finished
# column commands, so the whole thing is exercised on synthetic commands plus the fixtures; SDK-free.
# HOMESKZ_FIXTURES_DIR points the file-based cases at tests/fixtures/.
vw_add_test(ParseColumnMarkTests ParseColumnMarkTests.cpp CORE FIXTURES)

# Shear walls (src/parse/ShearWall): the braces (IfcMember 筋かい…) and the sheathing panels
# (IfcWall 面材…) turned into ONE linear-PIO command each, with both ends pulled onto the
# neighbouring column centres. Covers the wall-plane coordinate frame (axis taken from the
# extrusion direction, canonical axis orientation, rotating-caliper face width, the brace's
# rise direction), the two grouping rules (same-name braces = a crossed pair, same-line/same-run
# panels = the two faces of one wall) and the storey layer / floor-plan wiring. Exercised on
# synthetic STEP snippets and the checked-in fixtures; SDK-free.
# HOMESKZ_FIXTURES_DIR points the file-based cases at tests/fixtures/.
vw_add_test(ParseShearWallTests ParseShearWallTests.cpp)
target_link_libraries(ParseShearWallTests PRIVATE HomeskzIfcCore)
target_compile_definitions(ParseShearWallTests
						   PRIVATE "HOMESKZ_FIXTURES_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\"")

# Shared parse context (src/parse/Context): the per-import cache that lets every
# element's build*Commands reuse the same stories / grid centre / storey elements /
# loft-floor regions / roof planes instead of recomputing them. Proves the cached
# answers are stable across repeated calls AND identical to the uncached functions —
# the cache must not change behaviour. Exercised on the checked-in fixtures; SDK-free.
# HOMESKZ_FIXTURES_DIR points the file-based cases at tests/fixtures/.
vw_add_test(ParseContextTests ParseContextTests.cpp CORE FIXTURES)

# IFC read-result summary (src/parse/Summary): counts the homeskz-relevant entity
# types and formats them into the dialog text the import command shows. SDK-free —
# the counting + formatting is tested here; only the file picker and the alert
# itself live in the SDK menu command. HOMESKZ_FIXTURES_DIR points the file-based
# cases at the checked-in fixtures (tests/fixtures/).
vw_add_test(ParseSummaryTests ParseSummaryTests.cpp CORE FIXTURES)

# --- 並列実行（ctest -j）の詰め方 ---------------------------------------------
# ctest は COST の降順にテストを開始する。COST が無いと前回の実行の実測
# （build/Testing/Temporary/CTestCostData.txt）を使うが、CI は毎回まっさらな
# チェックアウトなのでそれが存在せず、登録順に開始することになる。**長いテストが
# 最後に始まると、その分だけ全体の終わりが後ろへずれる**（同じマシン・同じ -j4 での
# 実測: コストデータ有り 36 秒に対し、無しでは 60 秒）。
#
# そこで「重いものから始める」という情報だけを明示する。ctest が必要とするのは
# **相対的な重さ**であって秒数の正確さではないので、テストが少し伸び縮みしても
# 直し続ける必要はない（順位が入れ替わったときだけ見直せばよい）。ここに挙げるのは
# 長い方から数本だけで、残りは既定の 0 のまま——空いたスロットを埋める側に回る。
#
# 数字はローカルの逐次実行の実測秒（このディレクトリの各テストの Passed 表示）を
# 丸めたもの。CiWaitScriptTests だけは性質が違い、本物の締切を待つ**ほぼ純粋な
# 待ち時間**なので、CPU を占有しない代わりに何を足しても短くならない。いちばん先に
# 始めておくのが効く。
set_tests_properties(ParseRafterTests PROPERTIES COST 13)
set_tests_properties(LoaderTests PROPERTIES COST 12)
set_tests_properties(ParseFootingTests PROPERTIES COST 11)
set_tests_properties(ParseContextTests PROPERTIES COST 11)
set_tests_properties(ParseMemberTests PROPERTIES COST 10)
set_tests_properties(ParseJointTests PROPERTIES COST 9)
set_tests_properties(ParseColumnTests PROPERTIES COST 9)
set_tests_properties(ParseFireBraceTests PROPERTIES COST 8)
set_tests_properties(ParseAnchorBoltTests PROPERTIES COST 8)
set_tests_properties(UpdaterRobustnessTests PROPERTIES COST 6)
set_tests_properties(ParseFloorTests PROPERTIES COST 6)

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
		# COST は「いちばん先に始めさせる」ため（上記「並列実行の詰め方」）。実測 26 秒
		# のうちほとんどが待ち時間で、他のテストと並べても互いに邪魔をしない代わりに、
		# 後から始めるとその 26 秒がまるごと最後尾に足される。
		set_tests_properties(CiWaitScriptTests PROPERTIES TIMEOUT 900 COST 30)
	elseif(VW_REQUIRE_SCRIPT_TESTS)
		message(FATAL_ERROR "VW_REQUIRE_SCRIPT_TESTS is ON but 'bash' was not found: the CI-wait "
							"script tests (ci-wait.test.sh) cannot run.")
	endif()
endif()
