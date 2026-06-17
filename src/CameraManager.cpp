#include "CameraManager.h"

#include <chrono>

#include "OrbbecBackend.h"

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
    // 注册可用后端；接入新厂商（如迈德威视）时在此追加。
    backends_.push_back(std::make_shared<OrbbecBackend>());
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

void CameraManager::startCapture(int intervalMs, bool triggerMode) {
    // 先确保上一次的启动线程已结束
    abortStart_.store(true);
    joinStartThread();
    abortStart_.store(false);

    std::vector<std::shared_ptr<ICameraDevice>> devs = connectedDevices();
    if (devs.empty()) {
        return;
    }
    capturing_.store(true);

    if (intervalMs < 0) {
        intervalMs = 0;
    }

    startThread_ = std::thread([this, devs, intervalMs, triggerMode]() {
        for (size_t i = 0; i < devs.size(); ++i) {
            if (abortStart_.load()) {
                break;
            }
            if (i > 0 && intervalMs > 0) {
                // 分片睡眠，便于及时响应停止
                int remaining = intervalMs;
                while (remaining > 0 && !abortStart_.load()) {
                    const int step = remaining > 20 ? 20 : remaining;
                    std::this_thread::sleep_for(std::chrono::milliseconds(step));
                    remaining -= step;
                }
                if (abortStart_.load()) {
                    break;
                }
            }
            devs[i]->startStream(triggerMode);
        }
    });
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
