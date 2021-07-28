#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class thread_impl {
public:
    explicit thread_impl(uint32_t index) :
        thread([this]() { mainFunction(); }),
        isRequestedExit(false),
        index(index) {}

    bool IsCurrentThread() const {
        return std::this_thread::get_id() == thread.get_id();
    }

    template<typename T>
    bool TryPushTask(const T& task) {
        std::unique_lock<std::mutex> lock(mutex, std::defer_lock);
        if (lock.try_lock()) {
            localQueue.emplace_back(task);
            cond.notify_all();
            return true;
        }

        return false;
    }

    void RequestTerminate() {
        std::lock_guard<std::mutex> lock(mutex);
        isRequestedExit = true;
        cond.notify_all();
    }

private:
    void mainFunction() {
        while (!isRequestedExit) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cond.wait(lock, [this]() { return !localQueue.empty() || isRequestedExit; });

                if (isRequestedExit) {
                    break;
                }

                task = localQueue.back();
                localQueue.pop_back();
            }

            task();

            {
                std::unique_lock<std::mutex> lock(mutex);
                cond.notify_all();
            }
        }
    }

    std::thread thread;
    bool isRequestedExit;
    std::mutex mutex;
    std::condition_variable cond;
    std::deque<std::function<void()>> localQueue;
    uint32_t index;
};

class thread_pool {
public:
    explicit thread_pool(uint32_t count) {
        auto threadNum = count > 0 ? count : 2;
        for (uint32_t i = 0; i < threadNum; ++i) {
            impls.emplace_back(thread_impl(i));
        }
    }

    thread_pool() : thread_pool(std::thread::hardware_concurrency()) {}

    ~thread_pool() {
        for (auto& impl : impls) {
            impl.RequestTerminate();
        }
    }

    template<typename F>
    void AddTask(F&& func) {
        for (auto& impl : impls) {
            if (impl.TryPushTask(func)) {
                return;
            }
        }

        std::lock_guard<std::mutex> lock;
        globalQueue.push(std::forward<F>(func));
    }

private:
    std::mutex queueMutex;
    std::queue<std::function<void()>> globalQueue;
    std::vector<thread_impl> impls;
};

int main()
{
    std::cout << "Hello, Sandbox!" << std::endl;
}
