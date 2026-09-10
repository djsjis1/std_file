cmake_minimum_required(VERSION 3.15)
project(MyProject
    VERSION 1.0.0
    LANGUAGES CXX)

# 引入 MyFile 库(库自带 CMakeLists,目标 MyFile::MyFile)。
# C++23 要求与 MSVC /utf-8 由库目标以 INTERFACE 特性传播,无需在此重复设置
# 其他项目引入方式:add_subdirectory(path/to/MyFile) 后链接 MyFile::MyFile
add_subdirectory(MyFile)

# MSVC:中文注释与错误信息为 UTF-8(作用于本目录及子目录中其后创建的目标;库内部另有同样配置)
if(MSVC)
    add_compile_options(/utf-8)
endif()

#测试程序配置
add_executable(${PROJECT_NAME} main.cpp)

target_link_libraries(${PROJECT_NAME} PRIVATE
    MyFile::MyFile
)

# ==================== 单元测试(googletest) ====================
option(BUILD_TESTS "构建单元测试" ON)
if(BUILD_TESTS)
    # 嵌入方式集成:不安装、不构建 gtest 自带的测试
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    if(MSVC)
        # 与项目 CRT(/MD、/MDd)保持一致,避免链接错误
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    endif()

    # 优先使用 third/ 内置副本(离线可构建);third/ 不存在时回退 FetchContent 拉取固定版本
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third/googletest-main/CMakeLists.txt)
        add_subdirectory(third/googletest-main EXCLUDE_FROM_ALL)
    else()
        include(FetchContent)
        FetchContent_Declare(googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.14.0
        )
        FetchContent_MakeAvailable(googletest)
    endif()

    enable_testing()
    add_executable(file_tests tests/test_file.cpp)
    target_link_libraries(file_tests PRIVATE MyFile::MyFile GTest::gtest_main)

    add_test(NAME file_tests COMMAND file_tests)
    # 测试用相对路径临时目录,工作目录设为源码根
    set_tests_properties(file_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()
