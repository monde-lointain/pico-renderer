add_executable(geom_test geom_test.cc)
target_link_libraries(geom_test
    PRIVATE Renderer::Geom Renderer::Fixed renderer::harness gtest_main)
gtest_discover_tests(geom_test PROPERTIES LABELS geom)
