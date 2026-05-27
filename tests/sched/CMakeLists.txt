add_executable(sched_test sched_test.cc)
target_link_libraries(sched_test PRIVATE Renderer::Sched gtest_main)
gtest_discover_tests(sched_test PROPERTIES LABELS sched)
