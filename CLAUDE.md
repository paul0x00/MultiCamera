# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

基于 **C++ + Qt + Orbbec SDK 2.8.6** 的桌面端多相机控制软件，用于同时控制多组奥比中光 Gemini 215 结构光相机模组（枚举/连接、错峰启动采集、深度·红外·彩色预览、曝光/增益调节、软触发）。不是 git 仓库。

## 构建与运行

```bash
# macOS（本机，Qt 6.10.2）
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=/Users/paul/tool/Qt/6.10.2/macos
cmake --build build
./build/gemini215_viewer

# Windows（目标平台，MSVC2015 + Qt 5.12）
cmake -S . -B build -G "Visual Studio 14 2015" -A x64 -DCMAKE_PREFIX_PATH=C:/Qt/Qt5.12.12/5.12.12/msvc2015_64
cmake --build build --config Release
```

- 未传 `CMAKE_PREFIX_PATH` 时 CMakeLists 会兜底本机 Qt 路径（仅 macOS）。
- 没有测试套件、没有 lint 配置；验证手段是编译通过 + 启动运行。CMake 已导出 `compile_commands.json` 供 clangd 使用。
- 运行时依赖（dylib/dll、`extensions/`、`OrbbecSDKConfig.xml`）由 CMake 的 POST_BUILD 自动拷到可执行文件目录，无需手动处理。

## 架构

分层清晰，GUI 与厂商 SDK 严格隔离，多后端可插拔：

```text
main.cpp → MainWindow（Qt GUI，唯一的 Q_OBJECT/moc 类）
              └─ CameraManager（聚合各厂商后端；枚举、连接、错峰启动）
                    └─ ICameraBackend / ICameraDevice（抽象接口；公共类型在 CameraTypes.h）
                          └─ OrbbecBackend（持有 ob::Context；枚举/打开设备）
                                └─ OrbbecCameraDevice（单相机：ob::Pipeline、帧回调、参数、软触发）
                                      └─ FrameConverter（ob 帧 → QImage：深度伪彩 / IR 灰度 / 彩色解码）
```

- **MainWindow / CameraView**：纯 Qt，只依赖 `ICameraDevice`/`CameraTypes.h`，**不 include 任何厂商 SDK 头**。`CameraView` 故意不带 moc（用代码而非信号槽）。AUTOMOC=ON、AUTOUIC=OFF，界面全用代码构建——新增带信号槽的 QObject 子类时记得放进头文件让 AUTOMOC 处理。
- **CameraManager**：纯标准库 + 抽象接口。按 `DiscoveredDevice::vendor` 把连接请求路由到对应后端；接入新厂商（如迈德威视）= 实现 `ICameraBackend`+`ICameraDevice` 两个类，再在 `CameraManager` 构造函数里注册一行。
- **OrbbecBackend / OrbbecCameraDevice / FrameConverter**：Orbbec SDK + 标准库，**不碰任何 Qt GUI 对象**（仅用到 QImage 这种线程安全的值类型）。SDK 头只允许出现在这一层。

### 线程模型（不可违反的约定）

- 帧经 **SDK 内部线程**回调到达 `OrbbecCameraDevice::onFrameSet`，在回调里转成 `QImage`、加 `imageMutex_` 存入缓存；GUI 线程靠 `QTimer`（33ms，约 30fps）轮询 `latestImage()` 取副本显示。
- `onFrameSet` 全程包在 `try/catch(...)` 里——**C++ 异常绝不能逃逸进 SDK 的 C 回调边界**。新增回调内逻辑（含未来其他厂商实现）务必保持这一兜底。
- 对外状态用 `std::atomic`（state/frameCount/分辨率）或 mutex（错误串/图像缓存）保护。回调线程与 GUI 线程之外，`CameraManager` 还有一条**错峰启动后台线程**（按"采集间隔"依次启动各相机，分片睡眠以便及时响应停止）。
- 采集进行中（`isCapturing()`）禁用连接/断开按钮，避免后台启动线程与设备生命周期争用——改动按钮使能逻辑见 `updateButtonStates()`。

### SDK 交互要点

- **能力动态探测，不对型号硬编码**：曝光/增益/自动曝光的作用对象（深度/红外/彩色）通过 `isPropertySupported` 探测后动态列出；Orbbec 属性 ID 映射在 `OrbbecCameraDevice.cpp` 的匿名命名空间（`exposureProp/gainProp/autoExposureProp`）。
- **启动有回退**：先尝试启用全部可用流，失败则回退到仅深度流。
- **软触发**：`setMultiDeviceSyncConfig(SOFTWARE_TRIGGERING)` 启动 + `triggerCapture()` 触发；非触发模式用 `FREE_RUN`。
- 设备信息（name/serial/uid/pid/sensors）在 `OrbbecCameraDevice` 构造时一次性缓存，之后只读、线程安全。
- 析构顺序：先 `pipeline_.reset()` 再 `device_.reset()`，避免回调期间设备先于管线销毁。

## 跨平台约束（编辑代码前必读）

这是本项目最容易踩坑的地方——**本机只在 macOS/Qt6 下编译，但代码必须能在 Windows/Qt5.12/MSVC2015 下编译**，而 Qt6 头文件强制 C++17，所以本机用 C++17 标准编译，无法暴露 C++14/17 语法误用。

- **源码必须严格 C++11-clean**：不要用 `make_unique`（用 `make_shared`）、泛型 lambda、结构化绑定、`if constexpr` 等 C++14/17 语言特性。本机编过 ≠ Windows 编得过。
- **只用 Qt5/Qt6 共有的 API**。CMake 用 `find_package(QT NAMES Qt6 Qt5 ...)` 版本无关写法，并按 Qt 主版本切标准（Qt6→C++17，Qt5→C++11）。
- **两平台 SDK 头文件不可混用**：`third_party/orbbec/{macos,windows}/include/` 的 `Export.h` 分别用 `visibility` 与 `__declspec`。CMake 已按平台自动选择 include/lib，不要硬编码单一平台路径。
- Windows 相关已在 CMakeLists 内置：`/utf-8`（界面中文字面量，需 VS2015 Update 2+）、`NOMINMAX`/`WIN32_LEAN_AND_MEAN`、WIN32 子系统的 `Qt5::WinMain` 链接。
