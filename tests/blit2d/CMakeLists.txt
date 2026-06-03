add_executable(blit2d_test blit2d_test.cc)
target_link_libraries(blit2d_test PRIVATE Renderer::Blit2d gtest_main)
gtest_discover_tests(blit2d_test PROPERTIES LABELS blit2d)
