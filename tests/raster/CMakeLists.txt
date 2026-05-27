add_executable(raster_test raster_test.cc)
target_link_libraries(raster_test PRIVATE
    Renderer::Raster
    renderer::harness  # float oracle + golden-image framework
    gtest_main)
# Committed golden-image regression anchors for this lane (set GOLDEN_REGEN=1 to
# refresh). Lives in our owned glob; the harness golden dir is the harness lane's.
target_compile_definitions(raster_test PRIVATE
    RASTER_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")
gtest_discover_tests(raster_test PROPERTIES LABELS raster)
