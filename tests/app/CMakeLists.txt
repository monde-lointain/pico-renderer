include(GoogleTest)

add_executable(app_test
    app_test.cc
)
target_link_libraries(app_test PRIVATE renderer::app gtest_main)
target_include_directories(app_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
gtest_discover_tests(app_test)
