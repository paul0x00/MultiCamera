#ifndef CAMERA_TYPES_H
#define CAMERA_TYPES_H

#include <cstdint>
#include <string>

#include <QImage>

// 与具体相机 SDK 无关的公共类型，供界面、CameraManager 与各厂商后端共用。

// 单台相机的运行状态。
enum class CamState {
    Connected,   // 已打开设备，未采集
    Starting,    // 正在启动采集
    Streaming,   // 采集中
    Stopped,     // 已停止采集（仍连接）
    Error        // 出错
};

// 曝光/增益的作用对象（结构光相机通常调 Depth/IR；单流工业相机用 Industrial）。
enum class ExposureTarget { Depth, IR, Color, Industrial };

// 预览流类型。IRLeft/IRRight 仅用于触发保存时分别取左右红外帧，不参与预览选择。
enum class PreviewStream { Depth, IR, Color, IRLeft, IRRight };

// 整型属性的取值范围与当前值。具体单位由各实现定义，
// 界面只在 [min, max] 区间内取值，不对单位做任何假设。
struct PropertyRange {
    int cur  = 0;
    int max  = 0;
    int min  = 0;
    int step = 1;
    int def  = 0;
};

// 单帧的时间信息，用于跨厂商比较各相机的曝光时刻。
// 关键约定：hostEpochUs 必须落在同一条公共时间轴上——主机 std::chrono::system_clock
// 的 epoch 微秒。各厂商后端负责把自家设备时间戳（或 PTP/全局时间戳）换算到该轴，
// 这样不同厂商相机的 hostEpochUs 直接相减即为曝光时刻差。
struct FrameTiming {
    uint64_t hostEpochUs  = 0;      // 该帧曝光时刻（起点）在主机 epoch 的微秒值
    uint64_t captureRawUs = 0;      // 设备原始时间戳（µs），仅调试/溯源用，不跨厂商比较
    uint32_t exposureUs   = 0;      // 曝光时长（µs，best-effort）；需要中点时由使用方减 exposureUs/2
    bool     valid        = false;  // 是否取到有效时间信息
};

// 一次"触发保存"中单台相机抓到的成果。各厂商按需填充：
//  - Orbbec：color=彩色帧，left/right=左右红外（分时切 channel 取得）；timing 取彩色帧（主曝光）。
//  - 海康：color=彩色帧；timing 取该帧。
//  - 迈德威视：left/right=整帧切分的左右目灰度；timing 取该帧。
// 未产出的路保持空 QImage。ok=false 表示本次抓取失败（超时/异常）。
struct TriggerShot {
    QImage      color;
    QImage      left;
    QImage      right;
    FrameTiming timing;
    bool        ok = false;
};

// 枚举到的设备信息（用于设备列表展示，未必已连接）。
struct DiscoveredDevice {
    std::string vendor;          // 所属后端标识（见 ICameraBackend::vendor）
    std::string name;
    std::string serial;
    std::string uid;
    std::string connectionType;
    int         pid = 0;
};

// 作用对象的中文显示名。
inline const char *exposureTargetName(ExposureTarget t) {
    switch (t) {
    case ExposureTarget::Depth: return "深度";
    case ExposureTarget::IR:    return "红外";
    case ExposureTarget::Color: return "彩色";
    case ExposureTarget::Industrial: return "工业";
    }
    return "";
}

// 预览流的中文显示名。
inline const char *previewStreamName(PreviewStream s) {
    switch (s) {
    case PreviewStream::Depth:   return "深度";
    case PreviewStream::IR:      return "红外";
    case PreviewStream::Color:   return "彩色";
    case PreviewStream::IRLeft:  return "左红外";
    case PreviewStream::IRRight: return "右红外";
    }
    return "";
}

#endif  // CAMERA_TYPES_H
