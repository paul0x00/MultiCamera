#include "CameraManager.h"

#include <chrono>

#include "OrbbecBackend.h"
#ifdef _WIN32
#include "HikvisionBackend.h"
#include "MindvisionBackend.h"
#endif

namespace {
// 当前主机 system_clock 的 epoch 微秒（与各设备 FrameTiming 的公共时间轴一致）。
uint64_t nowEpochUs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}
}  // namespace

CameraManager::CameraManager() : abortStart_(false), capturing_(false) {
    // 注册可用后端；接入新厂商时在此追加。海康/迈德威视 SDK 仅 Windows 可用。
    backends_.push_back(std::make_shared<OrbbecBackend>());
#ifdef _WIN32
    backends_.push_back(std::make_shared<HikvisionBackend>());
    backends_.push_back(std::make_shared<MindvisionBackend>());
#endif
}

CameraManager::~CameraManager() {
    stopCapture();
    disconnectAll();
    backends_.clear();
}

std::string CameraManager::lastError() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return lastError_;
}

std::vector<DiscoveredDevice> CameraManager::refresh() {
    std::vector<DiscoveredDevice> result;
    std::string                   allErr;
    for (size_t i = 0; i < backends_.size(); ++i) {
        std::string                   err;
        std::vector<DiscoveredDevice> list = backends_[i]->discover(err);
        result.insert(result.end(), list.begin(), list.end());
        if (!err.empty()) {
            if (!allErr.empty()) {
                allErr += "；";
            }
            allErr += err;
        }
    }

    std::lock_guard<std::mutex> lk(mutex_);
    discovered_ = result;
    lastError_  = allErr;
    return result;
}

std::vector<DiscoveredDevice> CameraManager::discovered() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return discovered_;
}

ICameraBackend *CameraManager::backendFor(const std::string &vendor) const {
    for (size_t i = 0; i < backends_.size(); ++i) {
        if (vendor == backends_[i]->vendor()) {
            return backends_[i].get();
        }
    }
    return nullptr;
}

bool CameraManager::connect(const std::string &serial, std::string &err) {
    if (device(serial)) {
        return true;  // 已连接
    }
    // 从最近一次枚举结果中确定该设备所属的后端
    std::string vendor;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (size_t i = 0; i < discovered_.size(); ++i) {
            if (discovered_[i].serial == serial) {
                vendor = discovered_[i].vendor;
                break;
            }
        }
    }
    if (vendor.empty()) {
        err = "设备 " + serial + " 不在枚举结果中，请先刷新设备";
        return false;
    }
    ICameraBackend *backend = backendFor(vendor);
    if (!backend) {
        err = "未找到 " + vendor + " 后端";
        return false;
    }
    std::shared_ptr<ICameraDevice> camera = backend->open(serial, err);
    if (!camera) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mutex_);
        devices_.push_back(camera);
    }
    // 连接后对齐所有已打开设备的时钟（多机时间戳同步）。
    syncDeviceClocks();
    return true;
}

void CameraManager::syncDeviceClocks() {
    // backends_ 构造后只读，无需加锁。
    for (size_t i = 0; i < backends_.size(); ++i) {
        backends_[i]->syncDeviceClock();
    }
}

uint64_t CameraManager::calibrateClocks() {
    // 先做后端级时钟同步（如 Orbbec 上下文级对齐）。
    syncDeviceClocks();

    std::vector<std::shared_ptr<ICameraDevice>> devs = connectedDevices();
    if (devs.empty()) {
        return nowEpochUs();
    }
    // 并行屏障：各设备线程先就位，主线程统一取一次 hostRef 并释放，
    // 让所有 calibrateClock(hostRef) 用同一基准、近乎同瞬间执行。
    std::atomic<bool> go(false);
    std::atomic<int>  ready(0);
    std::atomic<uint64_t> hostRef(0);
    const int             n = static_cast<int>(devs.size());

    std::vector<std::thread> workers;
    workers.reserve(devs.size());
    for (size_t i = 0; i < devs.size(); ++i) {
        std::shared_ptr<ICameraDevice> dev = devs[i];
        workers.push_back(std::thread([&go, &ready, &hostRef, dev]() {
            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            dev->calibrateClock(hostRef.load());
        }));
    }
    while (ready.load() < n) {
        std::this_thread::yield();
    }
    const uint64_t tRef = nowEpochUs();
    hostRef.store(tRef);
    go.store(true, std::memory_order_release);

    for (size_t i = 0; i < workers.size(); ++i) {
        workers[i].join();
    }
    return tRef;
}

void CameraManager::disconnect(const std::string &serial) {
    std::shared_ptr<ICameraDevice> target;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (size_t i = 0; i < devices_.size(); ++i) {
            if (devices_[i]->serial() == serial) {
                target = devices_[i];
                devices_.erase(devices_.begin() + i);
                break;
            }
        }
    }
    if (target) {
        target->stopStream();  // 析构时也会停止，这里显式提前停止
        target.reset();
    }
}

int CameraManager::connectAll(std::vector<std::string> &errors) {
    std::vector<DiscoveredDevice> list = discovered();
    int                           ok   = 0;
    for (size_t i = 0; i < list.size(); ++i) {
        std::string err;
        if (connect(list[i].serial, err)) {
            ++ok;
        } else if (!err.empty()) {
            errors.push_back(err);
        }
    }
    return ok;
}

void CameraManager::disconnectAll() {
    std::vector<std::shared_ptr<ICameraDevice>> toRelease;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        toRelease.swap(devices_);
    }
    for (size_t i = 0; i < toRelease.size(); ++i) {
        toRelease[i]->stopStream();
    }
    toRelease.clear();
}

void CameraManager::joinStartThread() {
    if (startThread_.joinable()) {
        startThread_.join();
    }
}

void CameraManager::startCapture(bool triggerMode) {
    // 确保上一次的启动线程已结束
    abortStart_.store(true);
    joinStartThread();
    abortStart_.store(false);

    std::vector<std::shared_ptr<ICameraDevice>> devs = connectedDevices();
    if (devs.empty()) {
        return;
    }
    capturing_.store(true);

    // 一次性启动所有相机的采集（不再错峰；多机同步由时间戳标定 + 软触发屏障保证）。
    for (size_t i = 0; i < devs.size(); ++i) {
        devs[i]->startStream(triggerMode);
    }
}

void CameraManager::stopCapture() {
    abortStart_.store(true);
    joinStartThread();
    abortStart_.store(false);

    std::vector<std::shared_ptr<ICameraDevice>> devs = connectedDevices();
    for (size_t i = 0; i < devs.size(); ++i) {
        devs[i]->stopStream();
    }
    capturing_.store(false);
}

uint64_t CameraManager::triggerAll() {
    std::vector<std::shared_ptr<ICameraDevice>> devs = connectedDevices();

    // 仅对正在采集的相机触发
    std::vector<std::shared_ptr<ICameraDevice>> active;
    for (size_t i = 0; i < devs.size(); ++i) {
        if (devs[i]->isStreaming()) {
            active.push_back(devs[i]);
        }
    }
    if (active.empty()) {
        return nowEpochUs();
    }

    // 并行屏障：每台一个线程，先在 go 上自旋等待；待全部就位后主线程统一置位释放，
    // 使各相机的 trigger() 近乎同瞬间发出，把派发偏差压到亚毫秒级。
    std::atomic<bool> go(false);
    std::atomic<int>  ready(0);
    const int         n = static_cast<int>(active.size());

    std::vector<std::thread> workers;
    workers.reserve(active.size());
    for (size_t i = 0; i < active.size(); ++i) {
        std::shared_ptr<ICameraDevice> dev = active[i];
        workers.push_back(std::thread([&go, &ready, dev]() {
            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            dev->trigger();
        }));
    }

    // 等所有线程就位，再统一释放并记录派发时刻
    while (ready.load() < n) {
        std::this_thread::yield();
    }
    const uint64_t tTrigger = nowEpochUs();
    go.store(true, std::memory_order_release);

    for (size_t i = 0; i < workers.size(); ++i) {
        workers[i].join();
    }
    return tTrigger;
}

std::vector<std::shared_ptr<ICameraDevice>> CameraManager::connectedDevices() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return devices_;
}

std::shared_ptr<ICameraDevice> CameraManager::device(const std::string &serial) const {
    std::lock_guard<std::mutex> lk(mutex_);
    for (size_t i = 0; i < devices_.size(); ++i) {
        if (devices_[i]->serial() == serial) {
            return devices_[i];
        }
    }
    return std::shared_ptr<ICameraDevice>();
}

size_t CameraManager::connectedCount() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return devices_.size();
}
