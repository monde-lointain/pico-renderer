include(GoogleTest)

# Stream A smoke test: GoogleTest wiring + contract headers.
add_executable(smoke_test smoke_test.cc)
target_link_libraries(smoke_test PRIVATE
    renderer::game renderer::gfx renderer::app gtest_main)
gtest_discover_tests(smoke_test)

# Per-subsystem test suites own their own tests/<sub>/CMakeLists.txt (no shared
# file contention). EXISTS-guarded so the scaffold builds before they land.
foreach(sub game gfx app)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${sub}/CMakeLists.txt)
    add_subdirectory(${sub})
  endif()
endforeach()
