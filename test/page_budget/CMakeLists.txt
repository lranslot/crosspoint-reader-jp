add_executable(PageBudgetTest
  PageBudgetTest.cpp
)

target_include_directories(PageBudgetTest PRIVATE
  ${REPO_ROOT}/src/activities/reader
)

target_link_libraries(PageBudgetTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PageBudgetTest)
