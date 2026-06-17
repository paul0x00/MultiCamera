# Gemini 215 多相机控制台

基于 **C++11 + Qt + Orbbec SDK 2.8.6** 的桌面端多相机控制软件，用于同时控制多组奥比中光 Gemini 215 相机模组。

- 本机（macOS / Apple Silicon）使用已安装的 **Qt 6.10.2** 跑通；
- 源码按 **C++11** 规范编写，可在 **Windows + Qt 5.12** 下编译（见下方"跨平台说明"）。

## 功能

| 按钮 / 控件 | 说明 |
|---|---|
| 刷新设备 | 枚举当前连接的所有相机 |
| 连接选中 / 全部连接 | 打开设备并创建采集管线 |
| 断开选中 / 全部断开 | 停止采集并释放设备 |
| 开始采集 / 停止采集 | 启动 / 停止所有已连接相机的取流 |
| 采集间隔(ms) | **依次启动**相机列表中各相机模组的时间间隔（错峰启动，避免多结构光相机同时上电的带宽冲击） |
| 软触发模式 + 触发一次 | 勾选后以软触发模式启动；点"触发一次"向各相机发送一次抓拍命令 |
| 自动曝光 / 曝光 / 增益 | 针对所选作用对象（深度 / 红外 / 彩色，按设备能力自动列出）调节，支持"应用到全部" |
| 预览网格 | 每台相机一个预览块，可切换 深度(伪彩) / 红外(灰度) / 彩色，并显示状态、FPS、分辨率 |

## 项目结构

```
gemini215/
├── CMakeLists.txt              # 跨平台构建（Qt6/Qt5 版本无关；SDK 按平台分支）
├── README.md
├── .gitignore
├── src/                        # 全部应用源码
│   ├── main.cpp                # 程序入口
│   ├── MainWindow.{h,cpp}      # 主界面：设备表 / 预览网格 / 采集·参数·触发 / 日志
│   ├── CameraManager.{h,cpp}   # 持有 ob::Context；设备枚举、连接/断开、错峰启动
│   ├── CameraDevice.{h,cpp}    # 单相机：pipeline、帧回调、状态、曝光/增益/软触发
│   └── FrameConverter.{h,cpp}  # OB 帧 → QImage（深度伪彩 / IR 灰度 / 彩色解码）
└── third_party/orbbec/         # Orbbec SDK，按平台对称放置（自包含，便于整目录拷走）
    ├── macos/
    │   ├── include/libobsensor/    # macOS 头文件
    │   └── lib/                    # libOrbbecSDK.dylib + extensions/ + 配置 + cmake
    └── windows/
        ├── include/libobsensor/    # Windows 头文件（Export.h 用 __declspec，与 macOS 不同）
        ├── lib/OrbbecSDK.lib        # 导入库
        └── bin/OrbbecSDK.dll        # 运行时库
```

> 两平台 SDK 头文件**不可混用**：`Export.h` 在 macOS 用 `__attribute__((visibility))`、
> 在 Windows 用 `__declspec(dllimport)`。CMake 已按平台自动选择对应的 `include/` 与库。


## 构建与运行

### macOS（本机，Qt 6.10.2）

```bash
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=/Users/paul/tool/Qt/6.10.2/macos
cmake --build build
./build/gemini215_viewer
```

> CMakeLists 内置了本机 Qt 路径的兜底，未传 `CMAKE_PREFIX_PATH` 也能配置成功。
> 可执行文件通过 `@rpath` 自动定位 `lib/libOrbbecSDK.2.dylib`；`extensions/` 与
> `OrbbecSDKConfig.xml` 会在构建后自动拷到可执行文件目录。

### Windows（目标平台，MSVC2015 + Qt 5.12）

把整个工程目录拷到 Windows 后：

```bat
cmake -S . -B build -G "Visual Studio 14 2015" -A x64 ^
      -DCMAKE_PREFIX_PATH=C:/Qt/Qt5.12.12/5.12.12/msvc2015_64
cmake --build build --config Release
```

- `OrbbecSDK.dll` 会在构建后自动拷到 exe 同目录；若检测到 `windeployqt` 还会自动部署 Qt 运行库。
- 已为 MSVC 做的兼容处理（写在 CMakeLists 里，无需手动设置）：
  - `/utf-8`：让 MSVC 以 UTF-8 解析源码及执行字符集，正确处理界面中文字面量
    （**需 VS2015 Update 2 及以上，建议 Update 3**）；
  - `NOMINMAX` / `WIN32_LEAN_AND_MEAN`：避免 `windows.h` 的 `min/max` 宏破坏 `std::min/std::max`。
- 源码仅用 C++11 语言特性与 Qt5/Qt6 共有 API，MSVC2015 默认标准即可编译。


## 跨平台说明（重要）

| | 本机 macOS | Windows 目标 |
|---|---|---|
| Qt | 6.10.2 | 5.12 |
| C++ 标准 | C++17（Qt6 强制要求） | C++11 |

- **源码本身严格按 C++11 编写**（未使用 `make_unique`、泛型 lambda、结构化绑定等 C++14/17 语言特性），
  因此在 Windows + Qt5.12 下用 `-std=c++11` 可正常编译。
- 本机仅装有 Qt6，而 **Qt6 头文件强制需要 C++17**，故 CMake 会自动按 Qt 主版本切换标准：
  `Qt6 → C++17`、`Qt5 → C++11`。两种标准编译的是同一份 C++11 级别代码。
- 界面只使用 Qt5/Qt6 共有的 API；`CMakeLists` 用 `find_package(QT NAMES Qt6 Qt5 ...)`
  版本无关写法，两端共用同一份工程文件。

## 已验证 / 待真机验证

本机已验证（无相机环境）：
- ✅ Qt6.10 + C++17 下编译零错误、链接成功；
- ✅ 程序启动、主界面构建、30fps 刷新事件循环稳定运行；
- ✅ SDK Context 初始化、设备枚举（当前 0 台）无异常、无崩溃；
- ✅ dylib 经 rpath 正确加载。

需要插上 Gemini 215 后实测：
- 设备连接 / 采集取流 / 预览画面；
- 错峰启动间隔；曝光、增益、自动曝光的范围与生效；
- 软触发模式与"触发一次"（部分功能依赖具体型号对软触发同步模式的支持）。

## 设计要点

- **线程模型**：帧经 SDK 内部线程回调到达，在回调中转换为 `QImage` 并加锁缓存；
  GUI 线程用 `QTimer` 轮询取走副本显示。回调内 `try/catch(...)` 兜底，确保 C++ 异常
  绝不跨越 SDK 的 C 回调边界。
- **错峰启动**：采集启动在后台线程按"采集间隔"依次启动各相机，不阻塞界面；
  采集进行中禁用连接/断开，避免与启动线程争用设备生命周期。
- **健壮性**：启用全部可用流失败时自动回退到仅深度流；属性能力（曝光/增益/自动曝光）
  按设备实际支持情况动态探测，不对具体型号做硬编码假设。
