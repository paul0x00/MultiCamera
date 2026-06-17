#include "HikvisionBackend.h"

#include <cstring>

#include "HikvisionCameraDevice.h"
#include "MvCameraControl.h"

namespace {
// 把定长字节数组安全转成 std::string（截断到首个 '\0'）。
std::string fixedStr(const unsigned char *s, size_t cap) {
    if (!s) {
        return std::string();
    }
    size_t n = 0;
    while (n < cap && s[n] != 0) {
        ++n;
    }
    return std::string(reinterpret_cast<const char *>(s), n);
}

// 从枚举到的设备信息提取 DiscoveredDevice（USB3 / GigE 两种联合体分别取字段）。
bool fillDiscovered(const MV_CC_DEVICE_INFO *p, DiscoveredDevice &d) {
    if (!p) {
        return false;
    }
    if (p->nTLayerType == MV_USB_DEVICE) {
        const MV_USB3_DEVICE_INFO &u = p->SpecialInfo.stUsb3VInfo;
        d.name           = fixedStr(u.chModelName, sizeof(u.chModelName));
        d.serial         = fixedStr(u.chSerialNumber, sizeof(u.chSerialNumber));
        d.uid            = fixedStr(u.chDeviceGUID, sizeof(u.chDeviceGUID));
        d.connectionType = "USB3";
        d.pid            = 0;
    } else if (p->nTLayerType == MV_GIGE_DEVICE) {
        const MV_GIGE_DEVICE_INFO &g = p->SpecialInfo.stGigEInfo;
        d.name           = fixedStr(g.chModelName, sizeof(g.chModelName));
        d.serial         = fixedStr(g.chSerialNumber, sizeof(g.chSerialNumber));
        d.uid            = fixedStr(g.chUserDefinedName, sizeof(g.chUserDefinedName));
        d.connectionType = "GigE";
        d.pid            = 0;
    } else {
        return false;
    }
    if (d.serial.empty()) {
        return false;  // 无序列号无法按序列号打开
    }
    return true;
}
}  // namespace

HikvisionBackend::HikvisionBackend() {
    // SDK 初始化（多次调用安全）。
    MV_CC_Initialize();
}

std::vector<DiscoveredDevice> HikvisionBackend::discover(std::string &err) {
    std::vector<DiscoveredDevice> result;
    MV_CC_DEVICE_INFO_LIST        list;
    std::memset(&list, 0, sizeof(list));

    const int ret = MV_CC_EnumDevices(MV_USB_DEVICE | MV_GIGE_DEVICE, &list);
    if (ret != MV_OK) {
        err = "枚举海康设备失败 (0x" + std::to_string(ret) + ")";
        return result;
    }
    for (unsigned int i = 0; i < list.nDeviceNum; ++i) {
        DiscoveredDevice d;
        d.vendor = vendor();
        if (fillDiscovered(list.pDeviceInfo[i], d)) {
            result.push_back(d);
        }
    }
    return result;
}

std::shared_ptr<ICameraDevice> HikvisionBackend::open(const std::string &serial, std::string &err) {
    MV_CC_DEVICE_INFO_LIST list;
    std::memset(&list, 0, sizeof(list));
    if (MV_CC_EnumDevices(MV_USB_DEVICE | MV_GIGE_DEVICE, &list) != MV_OK) {
        err = "枚举海康设备失败";
        return std::shared_ptr<ICameraDevice>();
    }

    for (unsigned int i = 0; i < list.nDeviceNum; ++i) {
        DiscoveredDevice d;
        d.vendor = vendor();
        if (!fillDiscovered(list.pDeviceInfo[i], d) || d.serial != serial) {
            continue;
        }
        void *handle = nullptr;
        if (MV_CC_CreateHandle(&handle, list.pDeviceInfo[i]) != MV_OK || !handle) {
            err = "创建句柄失败: " + serial;
            return std::shared_ptr<ICameraDevice>();
        }
        const int ret = MV_CC_OpenDevice(handle, MV_ACCESS_Exclusive, 0);
        if (ret != MV_OK) {
            MV_CC_DestroyHandle(handle);
            err = "打开设备失败 (0x" + std::to_string(ret) + "): " + serial;
            return std::shared_ptr<ICameraDevice>();
        }
        return std::make_shared<HikvisionCameraDevice>(handle, d);
    }
    err = "未找到序列号为 " + serial + " 的海康设备";
    return std::shared_ptr<ICameraDevice>();
}
