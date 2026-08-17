add_executable(EncodingDetectTest
  EncodingDetectTest.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(EncodingDetectTest PRIVATE
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(EncodingDetectTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(EncodingDetectTest)
