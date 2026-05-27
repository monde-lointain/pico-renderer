add_executable(gfx_test
    assets_test.cc
    blit_test.cc
    font_test.cc
)
target_link_libraries(gfx_test PRIVATE renderer::gfx gtest_main)
gtest_discover_tests(gfx_test)
