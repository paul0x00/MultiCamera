#include "MindvisionBackend.h"

#include <cstring>

#ifdef _WIN32
#include <windows.h>  // CameraApi.h 依赖 Windows 类型
#endif
#include "CameraApi.h"
#include "MindvisionCameraDevice.h"

namespace {
// MindVision 设备信息字段为定长 char 数组，已是 '\0' 结尾，可直接构造 std::string。
std::string devName(const tSdkCameraDevInfo &d) {
    std::string s = d.acProductName;
    if (s.empty()) {
        s = d.acFriendlyName;
    }
    return s;
}
}  // namespace

MindvisionBackend::MindvisionBackend() {
    // SDK 初始化（多次调用安全）；0 = 英文（仅影响 SDK 内部字符串，本工程不依赖）。
    CameraSdkInit(1);
}

std::vector<DiscoveredDevice> MindvisionBackend::discover(std::string &err) {
    std::vector<DiscoveredDevice> result;

    tSdkCameraDevInfo list[16];
    int               count = 16;
    const int         rc    = CameraEnumerateDevice(list, &count);
    if (rc != CAMERA_STATUS_SUCCESS) {
        err = "枚举迈德威视设备失败 (" + std::to_string(rc) + ")";
        return result;
    }
    for (int i = 0; i < count; ++i) {
        DiscoveredDevice d;
        d.vendor         = vendor();
        d.name           = devName(list[i]);
        d.serial         = list[i].acSn;
        d.uid            = list[i].acFriendlyName;
        d.connectionType = list[i].acPortType;
        d.pid            = 0;
        if (!d.serial.empty()) {
            result.push_back(d);
        }
    }
    return result;
}

std::shared_ptr<ICameraDevice> MindvisionBackend::open(const std::string &serial, std::string &err) {
    tSdkCameraDevInfo list[16];
    int               count = 16;
    if (CameraEnumerateDevice(list, &count) != CAMERA_STATUS_SUCCESS) {
        err = "枚举迈德威视设备失败";
        return std::shared_ptr<ICameraDevice>();
    }

    for (int i = 0; i < count; ++i) {
        if (serial != list[i].acSn) {
            continue;
        }
        CameraHandle h  = 0;
        const int    rc = CameraInit(&list[i], -1, -1, &h);
        if (rc != CAMERA_STATUS_SUCCESS) {
            err = "打开迈德威视设备失败 (" + std::to_string(rc) + "): " + serial;
            return std::shared_ptr<ICameraDevice>();
        }

        // 读取能力，判定黑白/彩色并设定 ISP 输出格式。
        tSdkCameraCapbility cap;
        std::memset(&cap, 0, sizeof(cap));
        bool mono = false;
        if (CameraGetCapability(h, &cap) == CAMERA_STATUS_SUCCESS) {
            mono = (cap.sIspCapacity.bMonoSensor != 0);
        }
        CameraSetIspOutFormat(h, mono ? CAMERA_MEDIA_TYPE_MONO8 : CAMERA_MEDIA_TYPE_RGB8);

        DiscoveredDevice d;
        d.vendor         = vendor();
        d.name           = devName(list[i]);
        d.serial         = list[i].acSn;
        d.uid            = list[i].acFriendlyName;
        d.connectionType = list[i].acPortType;
        return std::make_shared<MindvisionCameraDevice>(static_cast<int>(h), d, mono);
    }
    err = "未找到序列号为 " + serial + " 的迈德威视设备";
    return std::shared_ptr<ICameraDevice>();
}
