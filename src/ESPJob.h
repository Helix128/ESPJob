#ifndef ESP_JOB_H
#define ESP_JOB_H

#define NOCALLBACK [](){}

#include <functional>
#include <memory>
#include <queue>
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#define ESPJOB_DEBUG 0 // debug off by default, set to 1 to enable job logging

#if ESPJOB_DEBUG
    #define JOB_LOG(fmt, ...) Serial.printf("[ESPJob] " fmt "\n", ##__VA_ARGS__)
#else
    #define JOB_LOG(fmt, ...)
#endif

#define LAMBDA(type) [&]()->type
#define CALLBACK(...) []( __VA_ARGS__ )

struct JobSystemConfig {
    uint32_t defaultStackSize = 4096;
    UBaseType_t defaultPriority = 1;
    BaseType_t defaultCore = tskNO_AFFINITY;
    uint16_t maxConcurrentTasks = 10;
    bool executeCallbacksInLoop = true;
};

enum class JobState {
    Pending,
    Running,
    Completed,
    Failed,
    Cancelled
};

class CallbackQueue {
public:
    static CallbackQueue& instance() {
        static CallbackQueue instance;
        return instance;
    }

    void enqueue(std::function<void()> callback) {
        if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
            queue.push(callback);
            xSemaphoreGive(mutex);
            JOB_LOG("Callback enqueued. Queue size: %d", queue.size());
        }
    }

    void process() {
        if (xSemaphoreTake(mutex, 0) == pdTRUE) {
            while (!queue.empty()) {
                auto callback = queue.front();
                queue.pop();
                xSemaphoreGive(mutex);
                
                // JOB_LOG("Processing callback..."); 
                callback();
                
                if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
                    return;
                }
            }
            xSemaphoreGive(mutex);
        }
    }

    size_t size() {
        size_t sz = 0;
        if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
            sz = queue.size();
            xSemaphoreGive(mutex);
        }
        return sz;
    }

private:
    CallbackQueue() {
        mutex = xSemaphoreCreateMutex();
    }

    ~CallbackQueue() {
        if (mutex) {
            vSemaphoreDelete(mutex);
        }
    }

    std::queue<std::function<void()>> queue;
    SemaphoreHandle_t mutex;
};

struct JobConfig {
    uint32_t stackSize = 0;
    UBaseType_t priority = 0;
    BaseType_t core = tskNO_AFFINITY;
    const char* name = nullptr;
    uint32_t timeoutMs = 0;
    bool executeInLoop = true;
};

class TaskHandle {
public:
    TaskHandle() : taskHandle(nullptr), state(JobState::Pending), 
                   cancelled(false), startTime(0), endTime(0) {}

    void setHandle(TaskHandle_t handle) { 
        taskHandle = handle; 
        state = JobState::Running;
        startTime = millis();
        JOB_LOG("Job started at %lu ms", startTime);
    }

    TaskHandle_t getHandle() const { return taskHandle; }
    
    JobState getState() const { return state; }
    
    void setState(JobState newState) { 
        state = newState;
        if (newState == JobState::Completed || 
            newState == JobState::Failed || 
            newState == JobState::Cancelled) {
            endTime = millis();
            JOB_LOG("Job ended at %lu ms. Duration: %lu ms", endTime, endTime - startTime);
        }
    }

    bool isCancelled() const { return cancelled; }
    
    void cancel() {
        cancelled = true;
        if (taskHandle != nullptr && state == JobState::Running) {
            setState(JobState::Cancelled);
            vTaskDelete(taskHandle);
            taskHandle = nullptr;
            JOB_LOG("Job cancelled manually");
        }
    }

    bool isRunning() const { 
        return state == JobState::Running && !cancelled; 
    }

    uint32_t getExecutionTime() const {
        if (startTime == 0) return 0;
        if (endTime == 0) return millis() - startTime;
        return endTime - startTime;
    }

private:
    TaskHandle_t taskHandle;
    JobState state;
    bool cancelled;
    uint32_t startTime;
    uint32_t endTime;
};

class JobHandle {
public:
    JobHandle() : handle(std::make_shared<TaskHandle>()), config() {}

    JobHandle(const JobHandle&) = delete;
    JobHandle& operator=(const JobHandle&) = delete;
    
    JobHandle(JobHandle&& other) noexcept : handle(std::move(other.handle)), 
                                                   config(other.config),
                                                   taskFunc(std::move(other.taskFunc)) {}
    
    JobHandle& operator=(JobHandle&& other) noexcept {
        if (this != &other) {
            handle = std::move(other.handle);
            config = other.config;
            taskFunc = std::move(other.taskFunc);
        }
        return *this;
    }

    template<typename Func, typename Callback>
    JobHandle(Func func, Callback callback, const JobConfig& cfg) 
        : handle(std::make_shared<TaskHandle>()), config(cfg) {
        using ResultType = decltype(func());
        taskFunc = [func, callback, h = handle, cfg]() {
            executeTask(func, callback, h, cfg, (ResultType*)nullptr);
        };
    }

    bool run() {
        if (!taskFunc) {
            JOB_LOG("ERROR: Job function is null");
            return false;
        }

        static uint32_t jobCounter = 0;
        char taskName[16];
        if (config.name == nullptr) {
            snprintf(taskName, sizeof(taskName), "Job_%lu", jobCounter++);
            config.name = taskName;
        }

        extern JobSystemConfig globalConfig;
        uint32_t stackSize = config.stackSize > 0 ? config.stackSize : globalConfig.defaultStackSize;
        UBaseType_t priority = config.priority > 0 ? config.priority : globalConfig.defaultPriority;
        BaseType_t core = config.core != tskNO_AFFINITY ? config.core : globalConfig.defaultCore;

        auto wrapper = [](void* param) {
            auto* func = static_cast<std::function<void()>*>(param);
            try {
                (*func)();
            } catch (...) {
                JOB_LOG("ERROR: Exception inside Job");
            }
            delete func;
            vTaskDelete(NULL);
        };

        auto* funcCopy = new std::function<void()>(taskFunc);
        TaskHandle_t taskHandle = nullptr;

        BaseType_t result;
        if (core != tskNO_AFFINITY) {
            result = xTaskCreatePinnedToCore(wrapper, config.name, stackSize, 
                                            funcCopy, priority, &taskHandle, core);
         } else {
            result = xTaskCreate(wrapper, config.name, stackSize, 
                               funcCopy, priority, &taskHandle);
        }

        if (result != pdPASS) {
            JOB_LOG("CRITICAL: Failed to create FreeRTOS Task");
            delete funcCopy;
            handle->setState(JobState::Failed);
            return false;
        }

        handle->setHandle(taskHandle);
        return true;
    }

    void cancel() {
        if (handle) {
            handle->cancel();
        }
    }

    JobState getState() const {
        return handle ? handle->getState() : JobState::Failed;
    }

    bool isRunning() const {
        return handle ? handle->isRunning() : false;
    }

    bool isCancelled() const {
        return handle ? handle->isCancelled() : false;
    }

    uint32_t getExecutionTime() const {
        return handle ? handle->getExecutionTime() : 0;
    }

private:
    std::shared_ptr<TaskHandle> handle;
    JobConfig config;
    std::function<void()> taskFunc;

    template<typename Func, typename Callback>
    static void executeTask(Func func, Callback callback, std::shared_ptr<TaskHandle> h, 
                          const JobConfig& cfg, void*) {
        
        if (h->isCancelled()) return;

        try {
            func();
            
            if (!h->isCancelled()) {
                h->setState(JobState::Completed);
                
                auto callbackWrapper = [callback]() {
                    callback();
                };

                if (cfg.executeInLoop) {
                    CallbackQueue::instance().enqueue(callbackWrapper);
                } else {
                    callbackWrapper();
                }
            }
        } catch (...) {
            JOB_LOG("Job failed with exception");
            h->setState(JobState::Failed);
        }
    }

    template<typename Func, typename Callback, typename ResultType>
    static void executeTask(Func func, Callback callback, std::shared_ptr<TaskHandle> h, 
                          const JobConfig& cfg, ResultType*) {
        
        if (h->isCancelled()) return;

        try {
            auto result = std::make_shared<ResultType>(func());
            
            if (!h->isCancelled()) {
                h->setState(JobState::Completed);
                
                auto callbackWrapper = [callback, result]() {
                    callback(*result);
                };

                if (cfg.executeInLoop) {
                    CallbackQueue::instance().enqueue(callbackWrapper);
                } else {
                    callbackWrapper();
                }
            }
        } catch (...) {
            JOB_LOG("Job failed with exception");
            h->setState(JobState::Failed);
        }
    }
};

JobSystemConfig globalConfig;

class Job {
public:
    static void setConfig(const JobSystemConfig& config) {
        globalConfig = config;
        JOB_LOG("Global Job config updated");
    }

    static void update() {
        if (globalConfig.executeCallbacksInLoop) {
            CallbackQueue::instance().process();
        }
    }

    static size_t pendingCallbacks() {
        return CallbackQueue::instance().size();
    }

    template<typename Func, typename Callback>
    static JobHandle Run(Func func, Callback cb) {
        JobConfig config;
        return Run(func, cb, config);
    }

    template<typename Func, typename Callback>
    static JobHandle Run(Func func, Callback cb, const JobConfig& config) {
        JobHandle task(func, cb, config);
        task.run();
        return task;
    }

    template<typename Func, typename Callback>
    static JobHandle Create(Func func, Callback cb, const JobConfig& config = JobConfig()) {
        return JobHandle(func, cb, config);
    }

    template<typename Func>
    static JobHandle FireAndForget(Func func, const JobConfig& config = JobConfig()) {
        auto noopCallback = []() {};
        return Run(func, noopCallback, config);
    }

    template<typename Func, typename Callback>
    static JobHandle RunAfter(uint32_t delayMs, Func func, Callback cb, 
                        const JobConfig& config = JobConfig()) {
        auto delayedFunc = [delayMs, func]() {
            vTaskDelay(pdMS_TO_TICKS(delayMs));
            return func();
        };
        return Run(delayedFunc, cb, config);
    }

    template<typename Func, typename Callback>
    static JobHandle RunOnCore(int core, Func func, Callback cb) {
        JobConfig config;
        config.core = core;
        return Run(func, cb, config);
    }

    template<typename Func, typename Callback>
    static JobHandle RunWithPriority(UBaseType_t priority, Func func, Callback cb) {
        JobConfig config;
        config.priority = priority;
        return Run(func, cb, config);
    }

    static int GetRunningCore() {
        return xPortGetCoreID();
    }
};

#endif