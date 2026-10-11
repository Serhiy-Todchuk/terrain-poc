#pragma once
#include <CesiumAsync/ITaskProcessor.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Fixed-size worker pool backing CesiumAsync::AsyncSystem's worker threads
// (tile decoding, mesh generation, GPU resource creation).
class TaskProcessor final : public CesiumAsync::ITaskProcessor {
public:
  explicit TaskProcessor(unsigned threadCount);
  ~TaskProcessor() override;

  void startTask(std::function<void()> f) override;

  size_t queuedTasks() const;
  size_t activeTasks() const { return _active.load(); }

private:
  void run(unsigned index);

  mutable std::mutex _mutex;
  std::condition_variable _cv;
  std::deque<std::function<void()>> _queue;
  std::vector<std::thread> _threads;
  std::atomic<size_t> _active{0};
  bool _stop = false;
};
