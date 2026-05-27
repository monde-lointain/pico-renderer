add_executable(raster_test raster_test.cc)
target_link_libraries(raster_test PRIVATE Renderer::Raster gtest_main)
gtest_discover_tests(raster_test PROPERTIES LABELS raster)
