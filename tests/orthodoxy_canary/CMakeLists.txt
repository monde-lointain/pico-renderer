# Orthodoxy enforcement-active canary.
#
# Engaged ONLY when ORTHODOXY_ACTIVE (guarded by the add_subdirectory in
# cmake/Orthodoxy.cmake), so local non-plugin builds never reach here.
#
# renderer_orthodoxy_canary compiles canary.cc with the plugin
# attached. The file contains deliberate, unsuppressed Orthodoxy violations, so
# the compile MUST fail when enforcement is live. EXCLUDE_FROM_ALL keeps it out
# of the normal build; only the WILL_FAIL ctest below drives it.
#
# This is a "this should NOT compile" check: ctest builds the target and
# expects a NONZERO exit. A green test => the build failed as required =>
# enforcement is proven live. A passing compile => enforcement is dead =>
# the test goes red.

add_executable(renderer_orthodoxy_canary EXCLUDE_FROM_ALL canary.cc)
target_link_libraries(renderer_orthodoxy_canary PRIVATE orthodoxy::plugin)

add_test(
  NAME renderer_orthodoxy_canary
  COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
          --target renderer_orthodoxy_canary --config $<CONFIG>)
set_tests_properties(renderer_orthodoxy_canary PROPERTIES
  WILL_FAIL TRUE
  LABELS "orthodoxy")
