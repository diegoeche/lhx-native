cmake_minimum_required(VERSION 3.20)
project(lhx_native LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
option(LHX_OPENGL "Build the optional OpenGL 3.3 scene backend" ON)
if(LHX_OPENGL)
  find_package(OpenGL QUIET)
endif()

set(LHX_ISO "${CMAKE_CURRENT_SOURCE_DIR}/bin/LHX.iso" CACHE FILEPATH "Owned LHX ISO image")
set(LHX_DATA "${CMAKE_CURRENT_SOURCE_DIR}/bin/dos" CACHE PATH "Extracted LHX game files")
set(LHX_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")

# Source discovery determines the number of C translation units, so do this
# during configuration. Reconfigure when the input or lifting tools change.
execute_process(COMMAND "${Python3_EXECUTABLE}" tools/prepare.py
  --iso "${LHX_ISO}" --output "${LHX_DATA}"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  RESULT_VARIABLE prepare_result)
if(NOT prepare_result EQUAL 0)
  message(FATAL_ERROR "Game extraction/unpacking failed")
endif()
execute_process(COMMAND "${Python3_EXECUTABLE}" tools/lift.py
  --exe "${LHX_DATA}/LHX.unpacked.EXE" --output "${LHX_GENERATED}"
  --pcrecomp "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pcrecomp"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  RESULT_VARIABLE lift_result)
if(NOT lift_result EQUAL 0)
  message(FATAL_ERROR "Native code generation failed")
endif()
execute_process(COMMAND "${Python3_EXECUTABLE}" tools/lift_audio.py
  --data-dir "${LHX_DATA}" --output "${LHX_GENERATED}"
  --pcrecomp "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pcrecomp"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  RESULT_VARIABLE audio_lift_result)
if(NOT audio_lift_result EQUAL 0)
  message(FATAL_ERROR "Native PC speaker driver generation failed")
endif()
execute_process(COMMAND "${Python3_EXECUTABLE}" tools/lift_missions.py
  --data-dir "${LHX_DATA}" --output "${LHX_GENERATED}"
  --pcrecomp "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pcrecomp"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  RESULT_VARIABLE mission_lift_result)
if(NOT mission_lift_result EQUAL 0)
  message(FATAL_ERROR "Native mission callback generation failed")
endif()
execute_process(COMMAND "${Python3_EXECUTABLE}" tools/lift_cockpits.py
  --data-dir "${LHX_DATA}" --output "${LHX_GENERATED}"
  --pcrecomp "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pcrecomp"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  RESULT_VARIABLE cockpit_lift_result)
if(NOT cockpit_lift_result EQUAL 0)
  message(FATAL_ERROR "Native cockpit drawing code generation failed")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${LHX_ISO}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare.py"
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/lift.py"
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/lhx.seeds"
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/lift_audio.py"
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/lift_missions.py"
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/lift_cockpits.py"
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/assets.py"
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pcrecomp/tools/disasm/decode16.py"
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pcrecomp/tools/lift/lift16.py")

file(GLOB lifted_sources CONFIGURE_DEPENDS "${LHX_GENERATED}/*.c")
file(GLOB runtime_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/runtime/*.c")
add_executable(lhx-native ${lifted_sources} ${runtime_sources})
target_include_directories(lhx-native PRIVATE runtime third_party/pcrecomp/runtime "${LHX_GENERATED}")
target_link_libraries(lhx-native PRIVATE PkgConfig::SDL2 m)
if(LHX_OPENGL AND OpenGL_FOUND)
  target_link_libraries(lhx-native PRIVATE OpenGL::GL)
  target_compile_definitions(lhx-native PRIVATE LHX_HAVE_OPENGL=1)
endif()
target_compile_definitions(lhx-native PRIVATE _DEFAULT_SOURCE)
target_compile_options(lhx-native PRIVATE -Wno-unused-label -Wno-unused-variable)

enable_testing()
add_executable(cpu-arithmetic-tests tests/cpu_arithmetic.c)
target_include_directories(cpu-arithmetic-tests PRIVATE third_party/pcrecomp/runtime)
add_test(NAME cpu-arithmetic COMMAND cpu-arithmetic-tests)
add_test(NAME prepare COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s tests -v)
set_tests_properties(prepare PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME native-boot COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/boot_smoke.py"
  --binary "$<TARGET_FILE:lhx-native>" --data-dir "${LHX_DATA}")
set_tests_properties(native-boot PROPERTIES TIMEOUT 50)
add_test(NAME native-menu COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/boot_smoke.py"
  --binary "$<TARGET_FILE:lhx-native>" --data-dir "${LHX_DATA}" --scenario mission-menu)
set_tests_properties(native-menu PROPERTIES TIMEOUT 50)
add_test(NAME native-flight COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/flight_smoke.py"
  --binary "$<TARGET_FILE:lhx-native>" --data-dir "${LHX_DATA}")
set_tests_properties(native-flight PROPERTIES TIMEOUT 100)
add_test(NAME native-tow COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/tow_smoke.py"
  --binary "$<TARGET_FILE:lhx-native>" --data-dir "${LHX_DATA}")
set_tests_properties(native-tow PROPERTIES TIMEOUT 120)
add_test(NAME native-render COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/render_smoke.py"
  --binary "$<TARGET_FILE:lhx-native>" --data-dir "${LHX_DATA}")
set_tests_properties(native-render PROPERTIES TIMEOUT 90)
