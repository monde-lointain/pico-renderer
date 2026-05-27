add_executable(sched_test sched_test.cc)
target_link_libraries(sched_test PRIVATE
    Renderer::Sched
    Renderer::rdr
    Renderer::DemoScene
    Renderer::Geom
    Renderer::Raster
    renderer::gfx
    gtest_main)
gtest_discover_tests(sched_test PROPERTIES LABELS sched)
