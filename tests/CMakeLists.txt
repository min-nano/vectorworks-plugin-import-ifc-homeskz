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
