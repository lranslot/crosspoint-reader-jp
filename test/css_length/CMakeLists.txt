add_executable(CssLengthTest
  CssLengthTest.cpp
)

target_include_directories(CssLengthTest PRIVATE
  ${REPO_ROOT}/lib/Epub
)

target_link_libraries(CssLengthTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CssLengthTest)
