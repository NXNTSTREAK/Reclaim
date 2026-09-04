cmake_minimum_required(VERSION 3.10)
project(RevenueRecoveryEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(recovery_engine 
    src/main.cpp
    src/db_manager.cpp
    src/scheduler.cpp
)

target_compile_features(recovery_engine PRIVATE cxx_std_17)
target_include_directories(recovery_engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

find_package(OpenSSL REQUIRED)
target_link_libraries(recovery_engine PRIVATE sqlite3 pthread OpenSSL::SSL OpenSSL::Crypto)