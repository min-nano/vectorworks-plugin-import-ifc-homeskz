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

function(vw_add_test name)
	add_executable(${name} ${ARGN})
	target_compile_features(${name} PRIVATE cxx_std_20)
	# Tests include the sources under "src" and the local "TestFramework.h".
	target_include_directories(${name} PRIVATE
		"${CMAKE_SOURCE_DIR}/src"
		"${CMAKE_CURRENT_SOURCE_DIR}"
	)
	if(VW_ENABLE_COVERAGE)
		target_compile_options(${name} PRIVATE
			-O0 -g --coverage -fprofile-arcs -ftest-coverage)
		target_link_options(${name} PRIVATE --coverage)
	endif()
	add_test(NAME ${name} COMMAND ${name})
endfunction()

# Header-only helpers.
vw_add_test(UpdaterParseTests
	UpdaterParseTests.cpp
)

# The flows, compiled with the fake host. src/UpdaterFlow.cpp includes no SDK
# header, so it links straight into the test binary.
vw_add_test(UpdaterFlowTests
	UpdaterFlowTests.cpp
	"${CMAKE_SOURCE_DIR}/src/UpdaterFlow.cpp"
)
