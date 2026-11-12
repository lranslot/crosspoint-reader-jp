add_executable(KinsokuAdjustTest KinsokuAdjustTest.cpp ${REPO_ROOT}/lib/Epub/Epub/CjkBreaks.cpp ${REPO_ROOT}/lib/Utf8/Utf8.cpp)
target_include_directories(KinsokuAdjustTest PRIVATE ${REPO_ROOT}/src/activities/reader ${REPO_ROOT}/lib/Epub ${REPO_ROOT}/lib/Epub/Epub ${REPO_ROOT}/lib/Utf8)
target_link_libraries(KinsokuAdjustTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(KinsokuAdjustTest)