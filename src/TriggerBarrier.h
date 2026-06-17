#ifndef TRIGGER_BARRIER_H
#define TRIGGER_BARRIER_H

#include <atomic>
#include <cstdint>
#include <thread>

// 多设备触发同步屏障：每台设备各开一线程，先各自完成"切软触发模式+重启取流"等准备，
// 准备好后调 arrive() 在屏障上自旋等待；待全部设备就位，控制线程调 releaseAndStamp()
// 统一记录派发时刻并释放所有线程，使各路 trigger() 近乎同瞬间发出，把派发偏差压到亚毫秒级。
//
// 用法：
//   控制线程：TriggerBarrier b; b.setExpected(n);
//            起 n 个工作线程，各线程内 b.arrive(); b.wait();（trigger 在 wait 返回后执行）
//            控制线程：b.waitAllReady(); uint64_t t = b.releaseAndStamp(now);
//
// 注意：每个工作线程必须恰好 arrive() 一次（即使准备失败也要 arrive，否则控制线程死等）。
class TriggerBarrier {
public:
    TriggerBarrier() : expected_(0), ready_(0), go_(false), dispatchUs_(0) {}

    void setExpected(int n) {
        expected_.store(n);
        ready_.store(0);
        go_.store(false);
        dispatchUs_.store(0);
    }

    // 工作线程：宣告就位（无论准备成功与否都必须调用一次）。
    void arrive() { ready_.fetch_add(1); }

    // 工作线程：在屏障上自旋，直到控制线程释放。返回派发时刻（主机 epoch µs）。
    uint64_t wait() const {
        while (!go_.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return dispatchUs_.load();
    }

    // 控制线程：等所有工作线程就位。
    void waitAllReady() const {
        const int n = expected_.load();
        while (ready_.load() < n) {
            std::this_thread::yield();
        }
    }

    // 控制线程：记录派发时刻并释放全部工作线程。返回该时刻。
    uint64_t releaseAndStamp(uint64_t nowUs) {
        dispatchUs_.store(nowUs);
        go_.store(true, std::memory_order_release);
        return nowUs;
    }

private:
    std::atomic<int>      expected_;
    std::atomic<int>      ready_;
    std::atomic<bool>     go_;
    std::atomic<uint64_t> dispatchUs_;
};

#endif  // TRIGGER_BARRIER_H
