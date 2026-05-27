add_executable(demo_scene_test demo_scene_test.cc)
target_link_libraries(demo_scene_test PRIVATE
    Renderer::rdr Renderer::DemoScene renderer::gfx gtest_main)
gtest_discover_tests(demo_scene_test PROPERTIES LABELS demo)
