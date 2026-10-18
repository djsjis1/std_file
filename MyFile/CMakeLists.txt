# ============================================================
# MyFile —— 跨平台文件操作库(独立 CMake 模块)
#
# 引入方式(任选其一):
#
# 1) add_subdirectory(最简单,推荐):
#      add_subdirectory(path/to/MyFile)
#      target_link_libraries(your_target PRIVATE MyFile::MyFile)
#
# 2) FetchContent:
#      include(FetchContent)
#      FetchContent_Declare(MyFile SOURCE_DIR path/to/MyFile)
#      FetchContent_MakeAvailable(MyFile)
#      target_link_libraries(your_target PRIVATE MyFile::MyFile)
#
# 3) install + find_package:
#      cmake --install build --prefix <prefix>
#      find_package(MyFile CONFIG REQUIRED)
#      target_link_libraries(your_target PRIVATE MyFile::MyFile)
# ============================================================

cmake_minimum_required(VERSION 3.15)

project(MyFile VERSION 1.0.0 LANGUAGES CXX)

add_library(MyFile STATIC
    platform.cpp
    file_info.cpp
    file_read.cpp
    file_write.cpp
    file_edit.cpp
    line_index.cpp
    mmap_file.cpp
    writer.cpp
    hash_glob.cpp
    watcher.cpp
    async.cpp
)
add_library(MyFile::MyFile ALIAS MyFile)

# 依赖 C++23(使用了 std::string::resize_and_overwrite 等特性),自动传播给使用者
target_compile_features(MyFile PUBLIC cxx_std_23)
 
# 头文件即在本目录,使用者无需额外配置 include 路径
target_include_directories(MyFile PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

# 源码为 UTF-8(BOM),MSVC 必须显式指定 /utf-8,避免中文注释被按 GBK 误解析
if(MSVC)
    target_compile_options(MyFile PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
endif()

# ==================== 安装规则(供 find_package 使用) ====================
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# 安装库目标和头文件
install(TARGETS MyFile EXPORT MyFileTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES file.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# 导出目标文件(包含 MyFile::MyFile 的 IMPORTED 定义)
install(EXPORT MyFileTargets
    FILE MyFileTargets.cmake
    NAMESPACE MyFile::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MyFile
)

# 从模板生成 MyFileConfig.cmake(包含 @PACKAGE_INIT@ 展开和依赖查找)
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/MyFileConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/MyFileConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MyFile
)

# 生成版本文件,使 find_package 支持 VERSION 参数做版本检查
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/MyFileConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

# 安装 Config 和 Version 文件
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/MyFileConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/MyFileConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MyFile
)
