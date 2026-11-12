add_executable(KinsokuRulesTest KinsokuRulesTest.cpp ${REPO_ROOT}/lib/Epub/Epub/CjkBreaks.cpp ${REPO_ROOT}/lib/Utf8/Utf8.cpp)
target_include_directories(KinsokuRulesTest PRIVATE ${REPO_ROOT}/lib/Epub ${REPO_ROOT}/lib/Epub/Epub ${REPO_ROOT}/lib/Utf8)
target_link_libraries(KinsokuRulesTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(KinsokuRulesTest)
