cmake_minimum_required(VERSION 3.25)

project(n7-order-book
    VERSION 0.1.0
    DESCRIPTION "High-performance low-latency order book in C++23"
    LANGUAGES CXX
)

# Standard C++23 configuration
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Generate compile_commands.json for clangd / IDE integration
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Source files
set(ORDER_BOOK_SOURCES
    src/order-book/book.cpp
)

set(ORDER_BOOK_HEADERS
    src/common/flat_hash_map.hpp
    src/common/object_pool.hpp
    src/common/utils.hpp
    src/order-book/book.hpp
    src/order-book/order.hpp
    src/order-book/price_level.hpp
    src/order-book/trade.hpp
)

# Main order book library target
add_library(order_book STATIC
    ${ORDER_BOOK_SOURCES}
    ${ORDER_BOOK_HEADERS}
)

add_library(n7::order_book ALIAS order_book)

target_include_directories(order_book
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(order_book PUBLIC cxx_std_23)

# Compiler warnings and optimizations
target_compile_options(order_book
    PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wnon-virtual-dtor
            -Wunused
        >
        $<$<CXX_COMPILER_ID:MSVC>:
            /W4
            /permissive-
        >
)

# Standalone executable target
add_executable(n7_order_book src/main.cpp)
target_link_libraries(n7_order_book PRIVATE n7::order_book)
target_compile_features(n7_order_book PUBLIC cxx_std_23)

# Clang-Format integration (honors global/local clang-format configs)
find_program(CLANG_FORMAT_EXE clang-format)
if(CLANG_FORMAT_EXE)
    file(GLOB_RECURSE ALL_FORMAT_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp
    )
    
    add_custom_target(format
        COMMAND ${CLANG_FORMAT_EXE} -i ${ALL_FORMAT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Formatting code with clang-format"
    )

    add_custom_target(format-check
        COMMAND ${CLANG_FORMAT_EXE} --dry-run --Werror ${ALL_FORMAT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Checking code format with clang-format"
    )
endif()

# Clang-Tidy integration
option(ENABLE_CLANG_TIDY "Run clang-tidy during compilation" OFF)
find_program(CLANG_TIDY_EXE clang-tidy)
if(CLANG_TIDY_EXE)
    if(ENABLE_CLANG_TIDY)
        set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
    endif()

    add_custom_target(tidy
        COMMAND ${CLANG_TIDY_EXE} -p ${CMAKE_BINARY_DIR} ${ORDER_BOOK_SOURCES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Running clang-tidy on source files"
    )
endif()
