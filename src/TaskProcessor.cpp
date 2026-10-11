#include "pch.h"
#include "TaskProcessor.h"

#include "Log.h"

TaskProcessor::TaskProcessor(unsigned threadCount) {
  threadCount = std::max(2u, threadCount);
  _threads.reserve(threadCount);
  for (unsigned i = 0; i < threadCount; ++i) {
    _threads.emplace_back([this, i] { run(i); });
  }
  LOG_I("Task processor started with {} worker threads", threadCount);
}

TaskProcessor::~TaskProcessor() {
  {
    std::lock_guard lock(_mutex);
    _stop = true;
  }
  _cv.notify_all();
  for (auto& t : _threads) {
    t.join();
  }
}

void TaskProcessor::startTask(std::function<void()> f) {
  {
    std::lock_guard lock(_mutex);
    _queue.push_back(std::move(f));
  }
  _cv.notify_one();
}

size_t TaskProcessor::queuedTasks() const {
  std::lock_guard lock(_mutex);
  return _queue.size();
}

void TaskProcessor::run(unsigned index) {
  std::wstring name = L"CesiumWorker" + std::to_wstring(index);
  SetThreadDescription(GetCurrentThread(), name.c_str());
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock lock(_mutex);
      _cv.wait(lock, [this] { return _stop || !_queue.empty(); });
      if (_stop && _queue.empty()) {
        return;
      }
      task = std::move(_queue.front());
      _queue.pop_front();
    }
    ++_active;
    try {
      task();
    } catch (const std::exception& e) {
      LOG_E("Unhandled exception in worker task: {}", e.what());
    } catch (...) {
      LOG_E("Unhandled non-standard exception in worker task");
    }
    --_active;
  }
}
