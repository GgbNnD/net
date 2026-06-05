cmake_minimum_required(VERSION 3.14)

# ============================================================
# P2P蓝牙文件传输工具 - 构建配置
# 项目名称: P2PFileTransfer
# C++标准: C++17
# 平台: Linux (BlueZ + D-Bus)
# ============================================================

project(P2PFileTransfer VERSION 2.0.0 LANGUAGES CXX)

# ----------------------------------------------------------
# C++标准设置
# ----------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ----------------------------------------------------------
# 构建输出目录
# ----------------------------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ----------------------------------------------------------
# 平台相关的库和编译选项
# ----------------------------------------------------------
# Linux: pthread + bluetooth + dbus-1
set(PLATFORM_LIBS pthread bluetooth dbus-1)

# ----------------------------------------------------------
# 收集所有源文件
# ----------------------------------------------------------
file(GLOB_RECURSE PROJECT_SOURCES
    "src/*.cpp"
    "src/*.c"
)

# ----------------------------------------------------------
# 创建可执行文件
# ----------------------------------------------------------
add_executable(${PROJECT_NAME} ${PROJECT_SOURCES})

# ----------------------------------------------------------
# 包含目录和链接库
# ----------------------------------------------------------
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/lib
)
target_link_libraries(${PROJECT_NAME} PRIVATE
    ${PLATFORM_LIBS}
)

# ----------------------------------------------------------
# 编译器警告选项
# ----------------------------------------------------------
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /utf-8)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -pedantic)
endif()

# ----------------------------------------------------------
# 调试和发布构建选项
# ----------------------------------------------------------
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0 -D_DEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
endif()

# ----------------------------------------------------------
# 打印构建信息
# ----------------------------------------------------------
message(STATUS "============================================")
message(STATUS "  项目名称: ${PROJECT_NAME}")
message(STATUS "  C++标准:  C++17")
message(STATUS "  构建类型: ${CMAKE_BUILD_TYPE}")
message(STATUS "  输出目录: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "  平台库:   ${PLATFORM_LIBS}")
message(STATUS "  JSON库:   nlohmann/json (本地 header-only)")
message(STATUS "============================================")
