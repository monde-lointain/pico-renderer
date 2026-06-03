add_executable(tex_test tex_test.cc)
# renderer::harness provides oracle_sample_texel (the float reference) so the
# fixed-point sampler can be validated against it within tolerance.
target_link_libraries(tex_test PRIVATE Renderer::Tex renderer::harness gtest_main)
gtest_discover_tests(tex_test PROPERTIES LABELS tex)
