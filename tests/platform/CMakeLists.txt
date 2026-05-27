# tests/platform/CMakeLists.txt — B.1-theta display-fork host tests.
# Host-testable parts only: RGB565 packing (display contract) and the
# scanline-race DMA watermark guard (pure integer logic). The PIO/COLMOD/DMA
# device path is verified on-target by stream eta's runner.
#
# NOTE: requires Lead to add `platform` to the foreach() in tests/CMakeLists.txt
# (Lead-owned). Until then this subdir is dormant.

add_executable(platform_test
    rgb565_test.cc
    scanrace_test.cc
)
target_link_libraries(platform_test PRIVATE renderer::platform_host gtest_main)
gtest_discover_tests(platform_test)
