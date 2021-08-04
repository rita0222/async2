#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <vector>

#include "fixed_size_function.h"

using functor_storage = fixed_size_function<void(), 128, construct_type::move>;

template<typename TResult>
struct task {
    task(std::shared_ptr<functor_storage>&& fn, std::future<TResult>&& ft) :
        functor(std::forward<std::shared_ptr<functor_storage>>(fn)),
        future(std::forward<std::future<TResult>>(ft)) {}

    std::shared_ptr<functor_storage> functor;
    std::shared_future<TResult> future;
};

template<typename TResult>
struct functor_dispatcher {
    template<typename F>
    static task<TResult> create_task(F&& functor) {
        std::promise<TResult> promise;
        return task<TResult>(std::make_shared<functor_storage>([f = std::move(functor), p = std::move(promise)]() mutable {
            auto result = f();
            p.set_value(result);
        }), std::move(promise.get_future()));
    }
};

template<>
struct functor_dispatcher<void> {
    template<typename F>
    static task<void> create_task(F&& functor) {
        std::promise<void> promise;
        return task<void>(std::make_shared<functor_storage>([f = std::move(functor), p = std::move(promise)]() mutable {
            f();
            p.set_value();
        }), std::move(promise.get_future()));
    }
};

template<typename F>
inline auto create_task(F&& functor) {
    return functor_dispatcher<typename std::invoke_result_t<F>>::create_task(std::forward<F>(functor));
}

class thread_impl {
public:
    explicit thread_impl(uint32_t index) :
        thread([this]() { mainFunction(); }),
        isRequestedExit(false),
        isSleeping(false),
        isQueueEmpty(false),
        index(index) {}

    ~thread_impl() {
        thread.join();
    }

    bool IsCurrentThread() const {
        return std::this_thread::get_id() == thread.get_id();
    }

    template<typename T>
    bool TryPushTask(const task<T>& task, bool forcePush) {
        auto expected = false;
        if (std::atomic_compare_exchange_strong(&isQueueEmpty, &expected, true)) {
            // 空っぽの場合はロックフリーでキュー追加可能
            localQueue.emplace_back(std::move(task.functor));
            cond.notify_all();
            return true;
        }

        std::unique_lock<std::mutex> lock(mutex, std::defer_lock);
        if (forcePush) {
            lock.lock();
            localQueue.emplace_back(std::move(task.functor));
            cond.notify_all();
            return true;
        }

        if (lock.try_lock()) {
            localQueue.emplace_back(std::move(task.functor));
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
            std::shared_ptr<functor_storage> currentTask;
            {
                std::unique_lock<std::mutex> lock(mutex);
                isSleeping = true;
                cond.wait(lock, [this]() { return !localQueue.empty() || isRequestedExit; });
                isSleeping = false;

                if (isRequestedExit) {
                    break;
                }

                currentTask = localQueue.back();
                localQueue.pop_back();
                if (localQueue.empty()) {
                    isQueueEmpty.store(true);
                }
            }

            currentTask->operator()();
        }
    }

    std::thread thread;
    bool isRequestedExit;
    bool isSleeping;
    std::atomic_bool isQueueEmpty;
    std::mutex mutex;
    std::condition_variable cond;
    std::deque<std::shared_ptr<functor_storage>> localQueue;

    uint32_t index;
};

class thread_pool {
public:
    explicit thread_pool(uint32_t count) {
        auto threadNum = count > 0 ? count : 2;
        for (uint32_t i = 0; i < threadNum; ++i) {
            impls.emplace_back(std::make_unique<thread_impl>(i));
        }
    }

    thread_pool() : thread_pool(std::thread::hardware_concurrency()) {}

    ~thread_pool() {
        for (auto& impl : impls) {
            impl->RequestTerminate();
        }
    }

    template<typename TResult>
    void AddTask(const task<TResult>& newTask) {
        for (auto& impl : impls) {
            if (impl->TryPushTask(newTask, false)) {
                return;
            }
        }

        std::lock_guard<std::mutex> lock(queueMutex);
        globalQueue.push(newTask.functor);
    }

private:
    std::mutex queueMutex;
    std::queue<std::shared_ptr<functor_storage>> globalQueue;
    std::vector<std::unique_ptr<thread_impl>> impls;
};

int main()
{
    std::cout << "Hello, Sandbox!" << std::endl;

    thread_pool pool(3);
    auto task1 = create_task([]() {
        std::cout << "I'm void() task." << std::endl;
        });
    auto task2 = create_task([]() {
        std::cout << "I'm int() task." << std::endl;
        return 42;
        });

    pool.AddTask(task1);
    pool.AddTask(task2);
    task1.future.wait();
    auto result = task2.future.get();
    std::cout << result << std::endl;
}
