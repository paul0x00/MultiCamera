cmake_minimum_required(VERSION 3.16)

project(gemini215_viewer LANGUAGES CXX)

# ---------------------------------------------------------------------------
# 基本设置
# ---------------------------------------------------------------------------
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC OFF)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
# 导出 compile_commands.json，便于 clangd / IDE 解析头文件
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 本机便捷：若未指定 CMAKE_PREFIX_PATH，且本机 Qt 安装存在，则自动加入。
# 在其它机器（如 Windows + Qt5.12）上该路径不存在，分支自动跳过，可用
#   -DCMAKE_PREFIX_PATH=<Qt路径>  覆盖。
if(NOT CMAKE_PREFIX_PATH AND APPLE AND EXISTS "/Users/paul/tool/Qt/6.10.2/macos")
    list(APPEND CMAKE_PREFIX_PATH "/Users/paul/tool/Qt/6.10.2/macos")
endif()
if(NOT CMAKE_PREFIX_PATH AND WIN32 AND EXISTS "F:/Qt6.9/6.9.0/mingw_64")
    list(APPEND CMAKE_PREFIX_PATH "F:/Qt6.9/6.9.0/mingw_64")
endif()

# ---------------------------------------------------------------------------
# Qt：版本无关。macOS 本机用 Qt6，Windows 目标用 Qt5.12，同一份 CMake 通吃。
# ---------------------------------------------------------------------------
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Gui Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui Widgets)

# Qt6 强制要求 C++17；Qt5 下保持 C++11（与 Windows/Qt5.12 目标一致）。
# 源码本身按 C++11 规范编写，故两种标准都能编译。
if(QT_VERSION_MAJOR GREATER_EQUAL 6)
    set(CMAKE_CXX_STANDARD 17)
else()
    set(CMAKE_CXX_STANDARD 11)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
message(STATUS "Using Qt${QT_VERSION_MAJOR} (${QT_VERSION}), C++${CMAKE_CXX_STANDARD}")

# ---------------------------------------------------------------------------
# Orbbec SDK：按平台选择头文件 / 库 / 运行时。
#   macOS  : 工程根目录 include/ + lib/libOrbbecSDK.dylib
#   Windows: third_party/orbbec/windows/{include, lib, bin}
# 两平台头文件不同（Export.h 的 dllimport vs visibility），不可混用。
# ---------------------------------------------------------------------------
set(ORBBEC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/orbbec")

if(APPLE)
    set(ORBBEC_INCLUDE_DIR "${ORBBEC_ROOT}/macos/include")
    set(ORBBEC_LIB_DIR     "${ORBBEC_ROOT}/macos/lib")
    set(ORBBEC_LIBRARY     "${ORBBEC_LIB_DIR}/libOrbbecSDK.dylib")
elseif(WIN32)
    set(ORBBEC_INCLUDE_DIR "${ORBBEC_ROOT}/windows/include")
    set(ORBBEC_LIB_DIR     "${ORBBEC_ROOT}/windows/lib")
    set(ORBBEC_LIBRARY     "${ORBBEC_LIB_DIR}/OrbbecSDK.lib")
    set(ORBBEC_DLL         "${ORBBEC_ROOT}/windows/bin/OrbbecSDK.dll")
else()
    message(FATAL_ERROR "暂不支持的平台（仅 macOS / Windows）")
endif()

if(NOT EXISTS "${ORBBEC_LIBRARY}")
    message(FATAL_ERROR "找不到 Orbbec SDK 库: ${ORBBEC_LIBRARY}")
endif()

# ---------------------------------------------------------------------------
# 工业相机 SDK（仅 Windows）：海康威视 MvCameraControl / 迈德威视 MVCAMSDK。
# 这两层的后端/设备类只在 Windows 下编译并注册（见 CameraManager 的 #ifdef _WIN32）。
# ---------------------------------------------------------------------------
if(WIN32)
    set(HIK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/hikvision/windows")
    set(MV_ROOT  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mindvision/windows")
    set(HIK_LIBRARY "${HIK_ROOT}/lib/MvCameraControl.lib")
    set(MV_LIBRARY  "${MV_ROOT}/lib/MVCAMSDK_X64.lib")
    set(HIK_DLL "${HIK_ROOT}/bin/MvCameraControl.dll")
    set(MV_DLL  "${MV_ROOT}/bin/MVCAMSDK_X64.dll")
    foreach(_lib "${HIK_LIBRARY}" "${MV_LIBRARY}")
        if(NOT EXISTS "${_lib}")
            message(FATAL_ERROR "找不到工业相机 SDK 库: ${_lib}")
        endif()
    endforeach()
endif()

# ---------------------------------------------------------------------------
# 目标
# ---------------------------------------------------------------------------
set(SOURCES
    src/main.cpp
    src/MainWindow.cpp
    src/CameraManager.cpp
    src/OrbbecBackend.cpp
    src/OrbbecCameraDevice.cpp
    src/FrameConverter.cpp
)
set(HEADERS
    src/MainWindow.h
    src/AppTheme.h
    src/CameraTypes.h
    src/ICameraDevice.h
    src/ICameraBackend.h
    src/CameraManager.h
    src/OrbbecBackend.h
    src/OrbbecCameraDevice.h
    src/FrameConverter.h
)

# 工业相机后端仅 Windows 编译（包含 Windows-only SDK 头文件）。
if(WIN32)
    list(APPEND SOURCES
        src/HikvisionBackend.cpp
        src/HikvisionCameraDevice.cpp
        src/MindvisionBackend.cpp
        src/MindvisionCameraDevice.cpp
    )
    list(APPEND HEADERS
        src/HikvisionBackend.h
        src/HikvisionCameraDevice.h
        src/MindvisionBackend.h
        src/MindvisionCameraDevice.h
    )
endif()

# WIN32：Windows 下作为 GUI 子系统程序（无控制台窗口）；该标记在 macOS 被忽略。
add_executable(gemini215_viewer WIN32 ${SOURCES} ${HEADERS})

target_include_directories(gemini215_viewer PRIVATE
    "${ORBBEC_INCLUDE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

if(WIN32)
    target_include_directories(gemini215_viewer PRIVATE
        "${HIK_ROOT}/include"
        "${MV_ROOT}/include"
    )
endif()

target_link_libraries(gemini215_viewer PRIVATE
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Widgets
    "${ORBBEC_LIBRARY}"
)

# MSVC：以 UTF-8 解析源码并设定执行字符集为 UTF-8，正确处理界面中文字面量
# （需 VS2015 Update 2 及以上；建议 VS2015 Update 3）
if(MSVC)
    target_compile_options(gemini215_viewer PRIVATE /utf-8)
endif()

# Windows：禁用 windows.h 的 min/max 宏，避免破坏 std::min/std::max
if(WIN32)
    target_compile_definitions(gemini215_viewer PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    # 工业相机 SDK 导入库（仅 Windows）
    target_link_libraries(gemini215_viewer PRIVATE
        "${HIK_LIBRARY}"
        "${MV_LIBRARY}"
    )
    # WIN32 子系统需要 qtmain 提供 WinMain 入口。
    # Qt5 显式链接 Qt5::WinMain；Qt6 由 Qt6::Core 对 WIN32 可执行文件自动链接 EntryPoint。
    if(TARGET Qt5::WinMain)
        target_link_libraries(gemini215_viewer PRIVATE Qt5::WinMain)
    endif()
endif()

# ---------------------------------------------------------------------------
# 运行时依赖
# ---------------------------------------------------------------------------
if(APPLE)
    # 让可执行文件在运行时能找到 dylib
    set_target_properties(gemini215_viewer PROPERTIES
        BUILD_RPATH   "${ORBBEC_LIB_DIR}"
        INSTALL_RPATH "${ORBBEC_LIB_DIR}"
    )
    # 拷贝默认配置与扩展（滤镜）到可执行文件目录，供 SDK 运行时加载
    if(EXISTS "${ORBBEC_LIB_DIR}/OrbbecSDKConfig.xml")
        add_custom_command(TARGET gemini215_viewer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${ORBBEC_LIB_DIR}/OrbbecSDKConfig.xml"
                    "$<TARGET_FILE_DIR:gemini215_viewer>/OrbbecSDKConfig.xml")
    endif()
    if(EXISTS "${ORBBEC_LIB_DIR}/extensions")
        add_custom_command(TARGET gemini215_viewer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${ORBBEC_LIB_DIR}/extensions"
                    "$<TARGET_FILE_DIR:gemini215_viewer>/extensions")
    endif()
endif()

if(WIN32)
    # 把 OrbbecSDK.dll 拷到 exe 同目录
    add_custom_command(TARGET gemini215_viewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ORBBEC_DLL}"
                "$<TARGET_FILE_DIR:gemini215_viewer>")
    # 把工业相机 SDK DLL 拷到 exe 同目录
    add_custom_command(TARGET gemini215_viewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${HIK_DLL}" "${MV_DLL}"
                "$<TARGET_FILE_DIR:gemini215_viewer>")
    # 若需要随包分发 Qt 运行库，可在生成后执行 windeployqt（按需开启）
    get_target_property(_qmake_loc Qt${QT_VERSION_MAJOR}::qmake IMPORTED_LOCATION)
    if(_qmake_loc)
        get_filename_component(_qt_bin_dir "${_qmake_loc}" DIRECTORY)
        find_program(WINDEPLOYQT_EXE windeployqt HINTS "${_qt_bin_dir}")
        if(WINDEPLOYQT_EXE)
            add_custom_command(TARGET gemini215_viewer POST_BUILD
                COMMAND "${WINDEPLOYQT_EXE}" --no-translations "$<TARGET_FILE:gemini215_viewer>")
        endif()
    endif()
endif()
