add_executable(blend_test blend_test.cc)
# renderer::harness supplies the float oracle (oracle_blend / oracle_fog_lerp)
# the fixed-point blend is validated against, plus oracle_unpack565.
target_link_libraries(blend_test PRIVATE Renderer::Blend renderer::harness gtest_main)
gtest_discover_tests(blend_test PROPERTIES LABELS blend)
