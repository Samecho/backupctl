cmake_minimum_required(VERSION 3.25)
project(backupctl VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "backupctl targets Linux. Use the project container on Windows.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BACKUPCTL_S3 "Build the AWS S3 adapter" ON)
set(BACKUPCTL_SANITIZER "" CACHE STRING "address or thread (separate builds)")

find_package(OpenSSL REQUIRED)
find_package(zstd CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_library(backup_core src/codec.cpp src/fs.cpp src/database.cpp src/local_store.cpp
    src/repository.cpp src/engine.cpp src/restore.cpp)
target_include_directories(backup_core PUBLIC include)
target_link_libraries(backup_core PUBLIC OpenSSL::Crypto zstd::libzstd_static
    unofficial::sqlite3::sqlite3 nlohmann_json::nlohmann_json Threads::Threads)
target_compile_options(backup_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

add_executable(backupctl src/main.cpp)
target_link_libraries(backupctl PRIVATE backup_core)
install(TARGETS backupctl RUNTIME DESTINATION bin)

if(BACKUPCTL_S3)
    find_package(AWSSDK CONFIG REQUIRED COMPONENTS s3 sts sso sso-oidc)
    target_sources(backup_core PRIVATE src/s3_store.cpp)
    target_compile_definitions(backup_core PRIVATE BACKUPCTL_S3=1)
    target_link_libraries(backup_core PUBLIC ${AWSSDK_LINK_LIBRARIES})
endif()

if(BACKUPCTL_SANITIZER STREQUAL "address")
    target_compile_options(backup_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(backup_core PUBLIC -fsanitize=address,undefined)
elseif(BACKUPCTL_SANITIZER STREQUAL "thread")
    target_compile_options(backup_core PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(backup_core PUBLIC -fsanitize=thread)
elseif(NOT BACKUPCTL_SANITIZER STREQUAL "")
    message(FATAL_ERROR "Unknown sanitizer: ${BACKUPCTL_SANITIZER}")
endif()

include(CTest)
if(BUILD_TESTING)
    target_compile_definitions(backup_core PRIVATE BACKUPCTL_TESTING=1)
    find_package(GTest CONFIG REQUIRED)
    add_executable(backup_tests tests/test_codec.cpp tests/test_storage.cpp tests/test_pool.cpp)
    target_link_libraries(backup_tests PRIVATE backup_core GTest::gtest_main)
    add_test(NAME unit COMMAND backup_tests)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME integration COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py $<TARGET_FILE:backupctl>)
    set_tests_properties(integration PROPERTIES TIMEOUT 180)
    add_test(NAME faults COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/faults.py $<TARGET_FILE:backupctl>)
    set_tests_properties(faults PROPERTIES TIMEOUT 300)
    add_test(NAME security COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/security.py $<TARGET_FILE:backupctl>)
    set_tests_properties(security PROPERTIES TIMEOUT 180)
    if(BACKUPCTL_S3)
        add_test(NAME s3_contract COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/s3_contract.py $<TARGET_FILE:backupctl>)
        set_tests_properties(s3_contract PROPERTIES TIMEOUT 180)
    endif()
endif()
