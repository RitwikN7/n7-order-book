add_executable(order_book_tests
    test_order_book.cpp
    test_latency_benchmark.cpp
)

target_link_libraries(order_book_tests
    PRIVATE
        n7::order_book
        GTest::gtest_main
)

target_include_directories(order_book_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src
)

target_compile_features(order_book_tests PUBLIC cxx_std_23)

include(GoogleTest)
gtest_discover_tests(order_book_tests)
