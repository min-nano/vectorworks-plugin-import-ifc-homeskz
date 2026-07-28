# CMake generated Testfile for 
# Source directory: /workspace/vectorworks-plugin-import-ifc-homeskz/tests
# Build directory: /workspace/vectorworks-plugin-import-ifc-homeskz/build-cov/tests
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[UpdaterParseTests]=] "/workspace/vectorworks-plugin-import-ifc-homeskz/build-cov/tests/UpdaterParseTests")
set_tests_properties([=[UpdaterParseTests]=] PROPERTIES  _BACKTRACE_TRIPLES "/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;72;add_test;/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;76;vw_add_test;/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;0;")
add_test([=[UpdaterFlowTests]=] "/workspace/vectorworks-plugin-import-ifc-homeskz/build-cov/tests/UpdaterFlowTests")
set_tests_properties([=[UpdaterFlowTests]=] PROPERTIES  _BACKTRACE_TRIPLES "/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;72;add_test;/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;80;vw_add_test;/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;0;")
add_test([=[UpdaterRobustnessTests]=] "/workspace/vectorworks-plugin-import-ifc-homeskz/build-cov/tests/UpdaterRobustnessTests")
set_tests_properties([=[UpdaterRobustnessTests]=] PROPERTIES  _BACKTRACE_TRIPLES "/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;72;add_test;/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;87;vw_add_test;/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;0;")
add_test([=[CoreDocumentTests]=] "/workspace/vectorworks-plugin-import-ifc-homeskz/build-cov/tests/CoreDocumentTests")
set_tests_properties([=[CoreDocumentTests]=] PROPERTIES  _BACKTRACE_TRIPLES "/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;72;add_test;/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;94;vw_add_test;/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;0;")
add_test([=[StepTests]=] "/workspace/vectorworks-plugin-import-ifc-homeskz/build-cov/tests/StepTests")
set_tests_properties([=[StepTests]=] PROPERTIES  _BACKTRACE_TRIPLES "/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;72;add_test;/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;100;vw_add_test;/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;0;")
add_test([=[LoaderTests]=] "/workspace/vectorworks-plugin-import-ifc-homeskz/build-cov/tests/LoaderTests")
set_tests_properties([=[LoaderTests]=] PROPERTIES  _BACKTRACE_TRIPLES "/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;72;add_test;/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;106;vw_add_test;/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;0;")
add_test([=[ParseGridTests]=] "/workspace/vectorworks-plugin-import-ifc-homeskz/build-cov/tests/ParseGridTests")
set_tests_properties([=[ParseGridTests]=] PROPERTIES  _BACKTRACE_TRIPLES "/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;72;add_test;/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;115;vw_add_test;/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;0;")
add_test([=[GeometryTests]=] "/workspace/vectorworks-plugin-import-ifc-homeskz/build-cov/tests/GeometryTests")
set_tests_properties([=[GeometryTests]=] PROPERTIES  _BACKTRACE_TRIPLES "/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;72;add_test;/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;126;vw_add_test;/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;0;")
add_test([=[ParseStoryTests]=] "/workspace/vectorworks-plugin-import-ifc-homeskz/build-cov/tests/ParseStoryTests")
set_tests_properties([=[ParseStoryTests]=] PROPERTIES  _BACKTRACE_TRIPLES "/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;72;add_test;/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;137;vw_add_test;/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;0;")
add_test([=[ParseStructuralClassTests]=] "/workspace/vectorworks-plugin-import-ifc-homeskz/build-cov/tests/ParseStructuralClassTests")
set_tests_properties([=[ParseStructuralClassTests]=] PROPERTIES  _BACKTRACE_TRIPLES "/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;72;add_test;/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;147;vw_add_test;/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;0;")
add_test([=[ParseSummaryTests]=] "/workspace/vectorworks-plugin-import-ifc-homeskz/build-cov/tests/ParseSummaryTests")
set_tests_properties([=[ParseSummaryTests]=] PROPERTIES  _BACKTRACE_TRIPLES "/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;72;add_test;/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;155;vw_add_test;/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;0;")
add_test([=[UpdaterScriptTests]=] "/usr/bin/bash" "/workspace/vectorworks-plugin-import-ifc-homeskz/tests/vw-update.test.sh")
set_tests_properties([=[UpdaterScriptTests]=] PROPERTIES  ENVIRONMENT "VW_REQUIRE_SCRIPT_TESTS=OFF" _BACKTRACE_TRIPLES "/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;176;add_test;/workspace/vectorworks-plugin-import-ifc-homeskz/tests/CMakeLists.txt;0;")
