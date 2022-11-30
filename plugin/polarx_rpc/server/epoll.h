//
// Created by zzy on 2022/7/5.
//

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <unistd.h>

#include "../common_define.h"
#include "../session/session.h"
#include "../utility/array_queue.h"
#include "../utility/atomicex.h"
#include "../utility/cpuinfo.h"
#include "../utility/perf.h"
#include "../utility/time.h"

#include "epoll_group_ctx.h"
#include "server_variables.h"
#include "timer_heap.h"

/** Note: for linux only */
#ifndef SO_REUSEPORT
#define SO_REUSEPORT 15
#endif

namespace polarx_rpc {

static constexpr uint32_t MAX_EPOLL_GROUPS = 128;
static constexpr uint32_t MAX_EPOLL_EXTRA_GROUPS = 32;
static constexpr uint32_t MAX_EPOLL_THREADS_PER_GROUP = 128;
static constexpr uint32_t MIN_EPOLL_WAIT_TOTAL_THREADS = 4;
static constexpr uint32_t MAX_EPOLL_WAIT_TOTAL_THREADS = 128;
static constexpr uint32_t MAX_EPOLL_EVENTS_PER_THREAD = 16;

static constexpr uint32_t MAX_EPOLL_TIMEOUT = 60 * 1000; /// 60s

static constexpr uint32_t MAX_TCP_KEEP_ALIVE = 7200;
static constexpr uint32_t MIN_TCP_LISTEN_QUEUE = 1;
static constexpr uint32_t MAX_TCP_LISTEN_QUEUE = 4096;

static constexpr uint32_t MIN_WORK_QUEUE_CAPACITY = 128;
static constexpr uint32_t MAX_WORK_QUEUE_CAPACITY = 4096;

class CmtEpoll;

/**
 * General interface for epoll callback.
 */
class CepollCallback {
public:
  virtual ~CepollCallback() = default;

  virtual void set_fd(int fd) = 0;

  /// for reclaim
  virtual void fd_registered() {}

  /// notify for adding reference
  virtual void pre_events() {}

  /// destruct the context when return false
  virtual bool events(uint32_t events, int index, int total) = 0;

  virtual bool send(const void *data, size_t length) { return false; }
};

/**
 * Timer/worker task.
 */
struct task_t final {
private:
  void *run_ctx_;
  void (*run_)(void *);
  void *del_ctx_;
  void (*del_)(void *);

public:
  task_t()
      : run_ctx_(nullptr), run_(nullptr), del_ctx_(nullptr), del_(nullptr) {}
  task_t(void *run_ctx, void (*run)(void *), void *del_ctx, void (*del)(void *))
      : run_ctx_(run_ctx), run_(run), del_ctx_(del_ctx), del_(del) {}

  task_t(const task_t &another) = default;
  task_t(task_t &&another) noexcept
      : run_ctx_(another.run_ctx_), run_(another.run_),
        del_ctx_(another.del_ctx_), del_(another.del_) {
    another.run_ctx_ = nullptr;
    another.run_ = nullptr;
    another.del_ctx_ = nullptr;
    another.del_ = nullptr;
  }

  ~task_t() = default;

  task_t &operator=(const task_t &another) = default;

  task_t &operator=(task_t &&another) noexcept {
    run_ctx_ = another.run_ctx_;
    run_ = another.run_;
    del_ctx_ = another.del_ctx_;
    del_ = another.del_;
    another.run_ctx_ = nullptr;
    another.run_ = nullptr;
    another.del_ctx_ = nullptr;
    another.del_ = nullptr;
    return *this;
  }

  explicit operator bool() const { return run_ != nullptr; }

  void call() const {
    if (run_ != nullptr)
      run_(run_ctx_);
  }

  void fin() const {
    if (del_ != nullptr)
      del_(del_ctx_);
  }
};

/// The inherited class should has private destructor to prevent alloc on stack.
template <class T> class Ctask {
  NO_COPY_MOVE(Ctask);

protected:
  Ctask() = default;
  virtual ~Ctask() = default;

private:
  static void run_routine(void *ctx) {
    auto task = reinterpret_cast<T *>(ctx);
    task->run();
  }

  static void del_routine(void *ctx) {
    auto task = reinterpret_cast<T *>(ctx);
    delete task;
  }

public:
  // Caution: Must call this function with object by new.
  task_t gen_task() {
    return {this, Ctask::run_routine, this, Ctask::del_routine};
  }
};

class CmtEpoll final {
  NO_COPY_MOVE(CmtEpoll);

private:
  /// group info
  const uint32_t group_id_;

  /// base epoll object
  int epfd_;

  /// timer task
  CmcsSpinLock timer_lock_;
  CtimerHeap<task_t> timer_heap_;

  /// work queue
  int eventfd_;
  CarrayQueue<task_t> work_queue_;

  /// worker wait counter
  std::atomic<intptr_t> wait_cnt_;
  std::atomic<intptr_t> loop_cnt_;

  /// extra data for epoll group
  epoll_group_ctx_t extra_ctx_;
  std::atomic<int64_t> last_cleanup_;

  /// affinity for dynamic threads
  bool with_affinity_;
  cpu_set_t cpus_{{}};
  std::string cores_str_;

  /// dynamic threads scale
  int base_thread_count_;
  std::atomic<int> stall_count_;
  std::atomic<int> worker_count_; /// work with epoll
  std::atomic<int> tasker_count_; /// work without epoll
  std::atomic<int64_t> last_scale_time_;
  std::atomic<int64_t> last_tasker_time_;
  std::mutex scale_lock_;
  std::atomic<int> session_count_; /// all session under this epoll

  /// watch dog deadlock check
  size_t last_head_;
  intptr_t last_loop_;

  static inline int nonblock(int fd, int set) {
    int flags;
    int r;
    do {
      r = ::fcntl(fd, F_GETFL);
    } while (UNLIKELY(r == -1 && errno == EINTR));

    if (UNLIKELY(r == -1))
      return -errno;

    /** Bail out now if already set/clear. */
    if (!!(r & O_NONBLOCK) == !!set)
      return 0;

    if (set != 0)
      flags = r | O_NONBLOCK;
    else
      flags = r & ~O_NONBLOCK;

    do {
      r = ::fcntl(fd, F_SETFL, flags);
    } while (UNLIKELY(r == -1 && errno == EINTR));

    if (UNLIKELY(r != 0))
      return -errno;
    return 0;
  }

  static inline int nodelay(int fd, int on) {
    if (UNLIKELY(::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) !=
                 0))
      return -errno;
    return 0;
  }

  static inline int keepalive(int fd, int on, unsigned int delay) {
    if (UNLIKELY(::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)) !=
                 0))
      return -errno;

#ifdef TCP_KEEPIDLE
    if (on &&
        ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &delay, sizeof(delay)) != 0)
      return -errno;
#endif

      /** Solaris/SmartOS, if you don't support keep-alive,
       * then don't advertise it in your system headers...
       */
#if defined(TCP_KEEPALIVE) && !defined(__sun)
    if (on && ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &delay,
                           sizeof(delay)) != 0)
      return -errno;
#endif
    return 0;
  }

  void loop(uint32_t group_id, uint32_t thread_id, bool base_thread,
            int affinity, bool epoll_wait, bool is_worker) {
    if (affinity >= 0) {
      auto thread = pthread_self();
      cpu_set_t cpu;
      CPU_ZERO(&cpu);
      auto iret = pthread_getaffinity_np(thread, sizeof(cpu), &cpu);
      if ((0 == iret && CPU_ISSET(affinity, &cpu)) || force_all_cores) {
        /// only set when this thread is allowed to run on it
        CPU_ZERO(&cpu);
        CPU_SET(affinity, &cpu);
        iret = pthread_setaffinity_np(thread, sizeof(cpu), &cpu);
        if (0 == iret)
          my_plugin_log_message(
              &plugin_info.plugin_info, MY_WARNING_LEVEL,
              "MtEpoll start worker thread %u:%u(%u,%u) bind to core %d.",
              group_id, thread_id, base_thread, epoll_wait, affinity);
        else
          my_plugin_log_message(&plugin_info.plugin_info, MY_WARNING_LEVEL,
                                "MtEpoll start worker thread %u:%u(%u,%u) bind "
                                "to core %d failed. %s",
                                group_id, thread_id, base_thread, epoll_wait,
                                affinity, std::strerror(errno));
      }
    } else if (!base_thread && with_affinity_) {
      /// auto bind for dynamic thread
      auto thread = pthread_self();
      auto iret = pthread_setaffinity_np(thread, sizeof(cpus_), &cpus_);
      if (0 == iret)
        my_plugin_log_message(&plugin_info.plugin_info, MY_WARNING_LEVEL,
                              "MtEpoll start dynamic worker thread "
                              "%u:%u(%u,%u) bind to cores %s.",
                              group_id, thread_id, base_thread, epoll_wait,
                              cores_str_.c_str());
      else
        my_plugin_log_message(
            &plugin_info.plugin_info, MY_WARNING_LEVEL,
            "MtEpoll start dynamic worker thread %u:%u(%u,%u) bind "
            "to cores %s failed. %s",
            group_id, thread_id, base_thread, epoll_wait, cores_str_.c_str(),
            std::strerror(errno));
    }

    std::vector<task_t> timer_tasks;
    CmcsSpinLock::mcs_spin_node_t timer_lock_node;
    Csession::init_thread_for_session();
    epoll_event events[MAX_EPOLL_EVENTS_PER_THREAD];
    while (true) {
      /// try pop and run task first
      while (true) {
        task_t t; /// pop one task at a time(more efficient with multi-thread)
        int64_t start_time = 0;
        if (enable_perf_hist)
          start_time = Ctime::steady_ns();

        /// get one from queue.
        work_queue_.pop(t);

        if (start_time != 0) {
          auto task_end_time = Ctime::steady_ns();
          auto work_queue_time = task_end_time - start_time;
          g_work_queue_hist.update(static_cast<double>(work_queue_time) / 1e9);
        }

        if (!t)
          break;
        t.call();
        t.fin();
      }

      if (!base_thread) {
        if (UNLIKELY(shrink_thread_pool(is_worker)))
          break;
      }

      /// limits the events
      auto max_events = epoll_events_per_thread;
      if (UNLIKELY(max_events <= 0))
        max_events = 1;
      else if (UNLIKELY(max_events > MAX_EPOLL_EVENTS_PER_THREAD))
        max_events = MAX_EPOLL_EVENTS_PER_THREAD;

      auto timeout = epoll_timeout;
      if (UNLIKELY(timeout <= 0))
        timeout = 1; /// busy waiting not allowed
      else if (UNLIKELY(timeout > MAX_EPOLL_TIMEOUT))
        timeout = MAX_EPOLL_TIMEOUT;

      /// only one thread with correct timeout to trigger timer is ok
      if (timer_lock_.try_lock(timer_lock_node)) {
        int64_t next_trigger;
        auto has_next = timer_heap_.peak(next_trigger);
        if (has_next) {
          /// adjust timeout
          auto now_time = Ctime::steady_ms();
          if (LIKELY(next_trigger - now_time > 0)) {
            timeout =
                std::min(timeout, static_cast<uint32>(next_trigger - now_time));
            DBG_LOG(
                ("polarx_rpc thread %u:%u enter epoll with timer timeout %ums",
                 group_id, thread_id, timeout));
          } else {
            timeout = 0;
            DBG_LOG(
                ("polarx_rpc thread %u:%u enter epoll with expired timer task",
                 group_id, thread_id));
          }
        } else {
          DBG_LOG(("polarx_rpc thread %u:%u enter epoll with no timer task",
                   group_id, thread_id));
        }
        timer_lock_.unlock(timer_lock_node);
      } else {
        DBG_LOG(
            ("polarx_rpc thread %u:%u enter epoll with failed timer lock race",
             group_id, thread_id));
      }

      wait_cnt_.fetch_add(1, std::memory_order_release);
      if (!work_queue_.empty()) {
        wait_cnt_.fetch_sub(1, std::memory_order_release);
        continue; /// dealing task first
      }
      int n;
      if (epoll_wait)
        n = ::epoll_wait(epfd_, events, static_cast<int>(max_events),
                         static_cast<int>(timeout));
      else {
        ::pollfd fds{eventfd_, POLLIN, 0};
        n = poll(&fds, 1, static_cast<int>(timeout));
        if (n > 0) {
          /// fake one
          assert(1 == n);
          events[0].data.fd = eventfd_;
          events[0].events = EPOLLIN;
        }
      }
      loop_cnt_.fetch_add(1, std::memory_order_relaxed);
      wait_cnt_.fetch_sub(1, std::memory_order_release);

      if (0 == n) {
        DBG_LOG(("polarx_rpc thread %u:%u leave epoll timeout, timeout %ums",
                 group_id, thread_id, timeout));
      } else {
        DBG_LOG(("polarx_rpc thread %u:%u leave epoll with %d events", group_id,
                 thread_id, n));
      }

      auto total = 0;
      for (auto i = 0; i < n; ++i) {
        if (events[i].data.fd == eventfd_) {
          /// consume the event fd as soon as possible
          /// which makes more threads notified if many tasks inserted
          uint64_t dummy;
          ::read(eventfd_, &dummy, sizeof(dummy));
          DBG_LOG(
              ("polarx_rpc thread %u:%u notified work", group_id, thread_id));
        } else {
          auto cb = reinterpret_cast<CepollCallback *>(events[i].data.ptr);
          assert(cb != nullptr);
          cb->pre_events();
          ++total;
        }
      }

      /// Note: move timer callback here before events dealing
      /// timer task only one thread is ok
      if (timer_lock_.try_lock(timer_lock_node)) {
        timer_tasks.clear();
        auto now_time = Ctime::steady_ms();
        task_t task;
        int32_t id;
        uint32_t type;
        while (timer_heap_.pop(now_time, task, id, type))
          timer_tasks.emplace_back(std::move(task));
        timer_lock_.unlock(timer_lock_node);

        /// run outside the lock
        for (const auto &t : timer_tasks) {
          t.call();
          t.fin();
        }
      }

      auto index = 0;
      for (auto i = 0; i < n; ++i) {
        if (events[i].data.fd == eventfd_)
          continue; /// ignore it
        auto cb = reinterpret_cast<CepollCallback *>(events[i].data.ptr);
        assert(cb != nullptr);
        auto bret = cb->events(events[i].events, index, total);
        if (!bret)
          delete cb;
        ++index;
      }

      /// do clean up on extra context
      auto last_time = last_cleanup_.load(std::memory_order_relaxed);
      auto now_time = Ctime::steady_ms();
      if (UNLIKELY(now_time - last_time > epoll_group_ctx_refresh_time)) {
        /// every 10s
        if (last_cleanup_.compare_exchange_strong(last_time, now_time)) {
          /// only one thread do this
          uintptr_t first = 0;
          for (auto i = 0; i < extra_ctx_.BUFFERED_REUSABLE_SESSION_COUNT;
               ++i) {
            std::unique_ptr<reusable_session_t> s;
            auto bret = extra_ctx_.reusable_sessions.pop(s);
            if (!bret)
              break;
            /// 10 min lifetime
            if (now_time - s->start_time_ms > shared_session_lifetime)
              s.reset(); /// release
            else {
              auto ptr_val = reinterpret_cast<uintptr_t>(s.get());
              extra_ctx_.reusable_sessions.push(std::move(s)); /// put it back
              if (0 == first)
                first = ptr_val;
              else if (ptr_val == first)
                break; /// all checked
            }
          }
        }
      }
    }
    Csession::deinit_thread_for_session();
  }

  explicit CmtEpoll(uint32_t group_id, size_t work_queue_depth)
      : group_id_(group_id), work_queue_(work_queue_depth), wait_cnt_(0),
        loop_cnt_(0), last_cleanup_(0), with_affinity_(true),
        base_thread_count_(0), stall_count_(0), worker_count_(0),
        tasker_count_(0), last_scale_time_(0), last_tasker_time_(0),
        session_count_(0), last_head_(0), last_loop_(0) {
    /// clear cpu set
    CPU_ZERO(&cpus_);

    /// init epoll
    epfd_ = ::epoll_create(0xFFFF); // 65535
    if (UNLIKELY(epfd_ < 0))
      throw std::runtime_error(std::strerror(errno));

    /// init eventfd
    eventfd_ = ::eventfd(0, EFD_NONBLOCK);
    if (UNLIKELY(eventfd_ < 0)) {
      ::close(epfd_);
      throw std::runtime_error(std::strerror(errno));
    }

    /// register it
    ::epoll_event event;
    event.data.fd = eventfd_;
    event.events = EPOLLIN | EPOLLET; /// only notify one
    auto iret = ::epoll_ctl(epfd_, EPOLL_CTL_ADD, eventfd_, &event);
    if (UNLIKELY(iret != 0)) {
      ::close(eventfd_);
      ::close(epfd_);
      throw std::runtime_error(std::strerror(errno));
    }
  }

  ~CmtEpoll() {
    /// never exit
    ::abort();
  }

  inline void init_thread(uint32_t group_id, uint32_t threads,
                          const std::vector<CcpuInfo::cpu_info_t> &affinities,
                          int base_idx, int epoll_wait_threads,
                          int epoll_wait_gap) {
    /// record threads count first
    base_thread_count_ = worker_count_ = static_cast<int>(threads);
    global_thread_count() += static_cast<int>(threads);

    std::ostringstream oss;
    oss << '[';
    for (uint32_t thread_id = 0; thread_id < threads; ++thread_id) {
      auto affinity = base_idx + thread_id < affinities.size()
                          ? affinities[base_idx + thread_id].processor
                          : -1;
      auto is_epoll_wait =
          0 == thread_id % epoll_wait_gap && --epoll_wait_threads >= 0;
      /// all thread is base thread when init
      std::thread thread(&CmtEpoll::loop, this, group_id, thread_id, true,
                         affinity, is_epoll_wait, true);
      thread.detach();
      /// record affinities
      if (affinity < 0)
        with_affinity_ = false;
      else if (!CPU_ISSET(affinity, &cpus_)) {
        CPU_SET(affinity, &cpus_); /// add to group set
        if (thread_id != 0)
          oss << ',';
        oss << affinity;
      }
    }
    if (with_affinity_) {
      oss << ']';
      cores_str_ = oss.str();
    }
  }

  static inline int get_core_number() {
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return (int)info.dwNumberOfProcessors;
#elif defined(__APPLE__)
    auto ncpu = 1;
    auto len = sizeof(ncpu);
    ::sysctlbyname("hw.activecpu", &ncpu, &len, nullptr, 0);
    return ncpu;
#else
    return ::get_nprocs();
#endif
  }

public:
  static inline std::atomic<int> &global_thread_count() {
    static std::atomic<int> g_cnt(0);
    return g_cnt;
  }

  static inline CmtEpoll **get_instance(size_t &instance_count) {
    static std::once_flag once;
    static CmtEpoll **inst = nullptr;
    static size_t inst_cnt = 0;
    if (UNLIKELY(nullptr == inst || 0 == inst_cnt)) {
      std::call_once(once, []() {
        /// recheck all variables to prevent part read when modifying
        auto threads = epoll_threads_per_group;
        if (UNLIKELY(threads <= 0))
          threads = 1;
        else if (UNLIKELY(threads > MAX_EPOLL_THREADS_PER_GROUP))
          threads = MAX_EPOLL_THREADS_PER_GROUP;

        auto groups = epoll_groups;
        auto base_groups = groups;
        if (groups <= 0) {
          auto cores = get_core_number();
          if (auto_cpu_affinity) {
            cpu_set_t cpu;
            CPU_ZERO(&cpu);
            auto iret =
                pthread_getaffinity_np(pthread_self(), sizeof(cpu), &cpu);
            if (0 == iret) {
              auto cpus = 0;
              for (auto i = 0; i < CPU_SETSIZE; ++i) {
                if (CPU_ISSET(i, &cpu))
                  ++cpus;
              }
              cores = cpus; /// at most cpus can run
            }
          }
          groups = cores / threads + (0 == cores % threads ? 0 : 1);
          if (groups < min_auto_epoll_groups)
            groups = (min_auto_epoll_groups / groups +
                      (0 == min_auto_epoll_groups % groups ? 0 : 1)) *
                     groups;
          base_groups = groups;
          /// dealing extra group
          auto extra = epoll_extra_groups;
          if (extra > MAX_EPOLL_EXTRA_GROUPS)
            extra = MAX_EPOLL_EXTRA_GROUPS;
          groups += extra;
        }
        if (UNLIKELY(base_groups > MAX_EPOLL_GROUPS))
          base_groups = MAX_EPOLL_GROUPS;
        if (UNLIKELY(groups > MAX_EPOLL_GROUPS))
          groups = MAX_EPOLL_GROUPS;

        std::vector<CcpuInfo::cpu_info_t> affinities;
        if (auto_cpu_affinity) {
          auto info_map = CcpuInfo::get_cpu_info();
          cpu_set_t cpu;
          CPU_ZERO(&cpu);
          auto iret = pthread_getaffinity_np(pthread_self(), sizeof(cpu), &cpu);
          if (0 == iret) {
            for (auto i = 0; i < CPU_SETSIZE; ++i) {
              auto it = info_map.find(i);
              if (CPU_ISSET(i, &cpu) ||
                  (force_all_cores && it != info_map.end())) {
                if (it == info_map.end())
                  /// no cpu info, just set to 0
                  affinities.emplace_back(CcpuInfo::cpu_info_t{i, 0, 0});
                else
                  affinities.emplace_back(it->second);
              }
            }
            /// if affinities not enough for base groups, just duplicate it
            if (base_groups * threads > affinities.size()) {
              auto duplicates = base_groups * threads / affinities.size();
              if (duplicates > 1) {
                std::vector<CcpuInfo::cpu_info_t> final_affinities;
                final_affinities.reserve(duplicates * affinities.size());
                for (size_t i = 0; i < duplicates; ++i) {
                  for (const auto &item : affinities)
                    final_affinities.emplace_back(item);
                }
                affinities = final_affinities;
              }
            }
            std::sort(affinities.begin(), affinities.end());
          }
        }

        auto total_epoll_wait_threads = max_epoll_wait_total_threads;
        if (0 == total_epoll_wait_threads)
          total_epoll_wait_threads = groups * threads;
        else if (UNLIKELY(total_epoll_wait_threads <
                          MIN_EPOLL_WAIT_TOTAL_THREADS))
          total_epoll_wait_threads = MIN_EPOLL_WAIT_TOTAL_THREADS;
        else if (UNLIKELY(total_epoll_wait_threads >
                          MAX_EPOLL_WAIT_TOTAL_THREADS))
          total_epoll_wait_threads = MAX_EPOLL_WAIT_TOTAL_THREADS;

        if (total_epoll_wait_threads < groups)
          /// at least one thread wait on epoll
          total_epoll_wait_threads = groups;

        auto epoll_wait_threads_per_group = 1;
        /// select some thread in epoll to do epoll_wait
        while (epoll_wait_threads_per_group < static_cast<int>(threads) &&
               (epoll_wait_threads_per_group + 1) * groups <=
                   total_epoll_wait_threads)
          ++epoll_wait_threads_per_group;
        auto epoll_wait_threads_gap = threads / epoll_wait_threads_per_group;

        auto work_queue_capacity = epoll_work_queue_capacity;
        if (UNLIKELY(work_queue_capacity < MIN_WORK_QUEUE_CAPACITY))
          work_queue_capacity = MIN_WORK_QUEUE_CAPACITY;
        else if (UNLIKELY(work_queue_capacity > MAX_WORK_QUEUE_CAPACITY))
          work_queue_capacity = MAX_WORK_QUEUE_CAPACITY;

        auto tmp = new CmtEpoll *[groups];
        for (uint32_t group_id = 0; group_id < groups; ++group_id) {
          tmp[group_id] = new CmtEpoll(group_id, work_queue_capacity);
          tmp[group_id]->init_thread(
              group_id, threads, affinities, group_id * threads,
              epoll_wait_threads_per_group, epoll_wait_threads_gap);
        }

        my_plugin_log_message(&plugin_info.plugin_info, MY_WARNING_LEVEL,
                              "MtEpoll start with %u groups with each group %u "
                              "threads. With %u thread bind to fixed CPU core",
                              groups, threads, affinities.size());

        inst_cnt = groups;
        inst = tmp;
      });
    }
    instance_count = inst_cnt;
    return inst;
  }

  inline const uint32_t &group_id() const { return group_id_; }

  /// 0 if success else -errno
  inline int add_fd(int fd, uint32_t events, CepollCallback *cb,
                    bool tcp = true) const {
    auto iret = nonblock(fd, 1);
    if (UNLIKELY(iret != 0))
      return iret;
    if (tcp && UNLIKELY((iret = nodelay(fd, 1)) != 0))
      return iret;
    auto tmp = tcp_keep_alive;
    if (UNLIKELY(tmp > MAX_TCP_KEEP_ALIVE))
      tmp = MAX_TCP_KEEP_ALIVE;
    if (tcp && tmp > 0 && UNLIKELY((iret = keepalive(fd, 1, tmp)) != 0))
      return iret;

    ::epoll_event event;
    event.data.ptr = cb;
    event.events = events;
    cb->set_fd(fd);
    DBG_LOG(("polarx_rpc epoll add fd %d", fd));
    iret = ::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &event);
    DBG_LOG(("polarx_rpc epoll add fd %d done ret %d", fd, iret));
    if (UNLIKELY(iret != 0))
      return -errno;
    cb->fd_registered();
    return 0;
  }

  /// 0 if success else -errno
  inline int reset_fd(int fd, uint32_t events, CepollCallback *cb) const {
    ::epoll_event event;
    event.data.ptr = cb;
    event.events = events;
    DBG_LOG(("polarx_rpc epoll mod fd %d", fd));
    auto iret = ::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &event);
    DBG_LOG(("polarx_rpc epoll mod fd %d done ret %d", fd, iret));
    return LIKELY(0 == iret) ? 0 : -errno;
  }

  /// 0 if success else -errno
  inline int del_fd(int fd) const {
    epoll_event dummy;
    ::memset(&dummy, 0, sizeof(dummy));
    DBG_LOG(("polarx_rpc epoll del fd %d", fd));
    auto iret = ::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, &dummy);
    DBG_LOG(("polarx_rpc epoll del fd %d done ret %d", fd, iret));
    return LIKELY(0 == iret) ? 0 : -errno;
  }

  static inline int check_port(uint16_t port) {
    auto fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (UNLIKELY(fd <= 0))
      return -errno;
    sockaddr_in address;
    ::memset(&address, 0, sizeof(address));
    if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr.s_addr) != 1) {
      auto err = errno;
      ::close(fd);
      return -err;
    }
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (0 == ::connect(fd, reinterpret_cast<struct sockaddr *>(&address),
                       sizeof(address))) {
      ::close(fd);
      return -EADDRINUSE;
    }
    auto err = errno;
    ::close(fd);
    return ECONNREFUSED == err ? 0 : -err;
  }

  /// 0 if success else -errno
  inline int listen_port(uint16_t port, CepollCallback *cb,
                         bool reuse = false) const {
    sockaddr_in address;
    ::memset(&address, 0, sizeof(address));
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    auto fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (UNLIKELY(fd <= 0))
      return -errno;
    int sock_op = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &sock_op, sizeof(sock_op));
    if (reuse)
      ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &sock_op, sizeof(sock_op));
    if (UNLIKELY(::bind(fd, (struct sockaddr *)&address, sizeof(address)) !=
                 0)) {
      auto err = errno;
      ::close(fd);
      return -err;
    }

    auto depth = tcp_listen_queue;
    if (UNLIKELY(depth < MIN_TCP_LISTEN_QUEUE))
      depth = MIN_TCP_LISTEN_QUEUE;
    else if (UNLIKELY(depth > MAX_TCP_LISTEN_QUEUE))
      depth = MAX_TCP_LISTEN_QUEUE;
    if (UNLIKELY(::listen(fd, depth) != 0)) {
      auto err = errno;
      ::close(fd);
      return -err;
    }
    int iret;
    if (UNLIKELY((iret = add_fd(fd, EPOLLIN | EPOLLET, cb)) != 0)) {
      ::close(fd);
      return iret;
    }
    return 0;
  }

  inline void push_trigger(task_t &&task, int64_t trigger_time) {
    int32_t id;
    int64_t last_time;

    /// reuse work queue spin conf
    CautoMcsSpinLock lck(timer_lock_, mcs_spin_cnt);
    if (UNLIKELY(!timer_heap_.peak(last_time)))
      last_time = trigger_time + 1;
    timer_heap_.push(std::forward<task_t>(task), trigger_time, id);
    lck.unlock();

    if (UNLIKELY(last_time - trigger_time >= 0)) {
      /// need notify to restart new thread to wait with smaller timeout
      uint64_t dummy = 1;
      ::write(eventfd_, &dummy, sizeof(dummy));
    }
  }

  inline bool push_work(task_t &&task) {
    auto bret = work_queue_.push(std::forward<task_t>(task));
    if (!bret)
      return false;

    /// read with write barrier
    auto waiting = wait_cnt_.fetch_add(0, std::memory_order_acq_rel);
    if (waiting > 0) {
      /// notify if some one is in epoll
      uint64_t dummy = 1;
      ::write(eventfd_, &dummy, sizeof(dummy));
    }
    return true;
  }

  inline epoll_group_ctx_t &get_extra_ctx() { return extra_ctx_; }

  inline void add_stall_count() { ++stall_count_; }

  inline void sub_stall_count() { --stall_count_; }

  /// thread pool auto scale and shrink

  inline bool worker_stall_since_last_check() {
    auto head = work_queue_.head();
    if (LIKELY(head != last_head_)) {
      last_head_ = head;
      return false;
    }
    /// consumer not moved
    auto tail = work_queue_.tail();
    if (head != tail) /// not empty
      return true;
    /// check epoll wait exists
    auto loop = loop_cnt_.load(std::memory_order_acquire);
    auto waits = wait_cnt_.load(std::memory_order_acquire);
    if (likely(waits > 0)) {
      last_loop_ = loop;
      return false;
    }
    if (LIKELY(loop != last_loop_)) {
      last_loop_ = loop;
      return false;
    }
    return true; /// empty task but no thread wait on epoll
  }

  inline void force_scale_thread_pool() {
    last_scale_time_.store(Ctime::steady_ms(), std::memory_order_release);

    std::lock_guard<std::mutex> lck(scale_lock_);

    if (worker_count_.load(std::memory_order_acquire) >=
        session_count_.load(std::memory_order_acquire) + base_thread_count_) {
      if (enable_thread_pool_log)
        my_plugin_log_message(
            &plugin_info.plugin_info, MY_WARNING_LEVEL,
            "MtEpoll %u thread pool force scale over limit, worker %d tasker "
            "%d, session %d. Total threads %d.",
            group_id_, worker_count_.load(std::memory_order_acquire),
            tasker_count_.load(std::memory_order_acquire),
            session_count_.load(std::memory_order_acquire),
            global_thread_count().load(std::memory_order_acquire));
      return; /// ignore if worker more than session
    }

    /// force scale one thread
    ++worker_count_;
    ++global_thread_count();
    std::thread thread(&CmtEpoll::loop, this, group_id_, 999, false, -1, true,
                       true);
    thread.detach();

    if (enable_thread_pool_log)
      my_plugin_log_message(
          &plugin_info.plugin_info, MY_WARNING_LEVEL,
          "MtEpoll %u thread pool force scale to worker %d tasker %d. Total "
          "threads %d.",
          group_id_, worker_count_.load(std::memory_order_acquire),
          tasker_count_.load(std::memory_order_acquire),
          global_thread_count().load(std::memory_order_acquire));
  }

  inline const std::atomic<int> &session_count() const {
    return session_count_;
  }

  inline std::atomic<int> &session_count() { return session_count_; }

  inline void balance_tasker() {
    auto pending = work_queue_.length();
    auto workers = worker_count_.load(std::memory_order_acquire);
    assert(workers >= 0);
    auto taskers = tasker_count_.load(std::memory_order_acquire);
    assert(taskers >= 0);

    auto multiply = epoll_group_tasker_multiply;
    auto multiply_low = multiply / 2;
    if (multiply_low < 1)
      multiply_low = 1;

    if (pending * 2 > work_queue_.capacity() ||
        pending > multiply_low * (workers + taskers)) {
      last_tasker_time_.store(Ctime::steady_ms(), std::memory_order_release);

      if (pending * 2 <= work_queue_.capacity() &&
          pending <= multiply * (workers + taskers))
        return; /// still under thresh

      /// need balance
      std::lock_guard<std::mutex> lck(scale_lock_);

      workers = worker_count_.load(std::memory_order_acquire);
      assert(workers >= 0);
      taskers = tasker_count_.load(std::memory_order_acquire);
      assert(taskers >= 0);
      auto sessions = session_count_.load(std::memory_order_acquire);
      assert(taskers >= 0);

      if (workers + taskers < sessions &&
          workers + taskers < static_cast<int>(pending)) {
        auto extend = (pending - workers - taskers) / multiply;
        if (0 == extend)
          extend = 1;
        if (extend > epoll_group_tasker_extend_step)
          extend = epoll_group_tasker_extend_step;

        tasker_count_ += extend;
        global_thread_count() += extend;
        for (size_t i = 0; i < extend; ++i) {
          std::thread thread(&CmtEpoll::loop, this, group_id_, 999, false, -1,
                             enable_epoll_in_tasker, false);
          thread.detach();
        }

        if (enable_thread_pool_log)
          my_plugin_log_message(
              &plugin_info.plugin_info, MY_WARNING_LEVEL,
              "MtEpoll %u thread pool tasker scale to %d, worker %d. Total "
              "threads %d.",
              group_id_, tasker_count_.load(std::memory_order_acquire),
              worker_count_.load(std::memory_order_acquire),
              global_thread_count().load(std::memory_order_acquire));
      }
    }
  }

  inline void try_scale_thread_pool(int wait_type) {
    auto thresh = static_cast<int>(epoll_group_thread_scale_thresh);
    if (UNLIKELY(thresh < 0))
      thresh = 0;
    else if (UNLIKELY(thresh >= base_thread_count_)) {
      thresh = base_thread_count_ - 1;
      assert(thresh >= 0);
    }
    auto stalled = stall_count_.load(std::memory_order_acquire);
    assert(stalled >= 0);
    auto workers = worker_count_.load(std::memory_order_acquire);
    assert(workers >= 0);
    auto prefer_thread_count =
        static_cast<int>(base_thread_count_ + epoll_group_dynamic_threads);

    /// refresh the last time if needed
    if (stalled > workers - base_thread_count_ + thresh)
      last_scale_time_.store(Ctime::steady_ms(), std::memory_order_release);
    else if (workers >= prefer_thread_count) {
      if (stalled > workers / 4)
        last_scale_time_.store(Ctime::steady_ms(), std::memory_order_release);
      return; /// do nothing
    }

    /// do scale if needed(recheck in lock)
    std::lock_guard<std::mutex> lck(scale_lock_);
    stalled = stall_count_.load(std::memory_order_acquire);
    assert(stalled >= 0);
    workers = worker_count_.load(std::memory_order_acquire);
    assert(workers >= 0);

    if (workers >=
        session_count_.load(std::memory_order_acquire) + base_thread_count_) {
      if (enable_thread_pool_log)
        my_plugin_log_message(
            &plugin_info.plugin_info, MY_WARNING_LEVEL,
            "MtEpoll %u thread pool scale over limit, worker %d tasker %d, "
            "session %d. Total threads %d.",
            group_id_, worker_count_.load(std::memory_order_acquire),
            tasker_count_.load(std::memory_order_acquire),
            session_count_.load(std::memory_order_acquire),
            global_thread_count().load(std::memory_order_acquire));
      return; /// ignore if worker more than session
    }

    auto scaled = false;
    if (stalled > workers - base_thread_count_ + thresh) {
      /// need extra thread to handle new request
      ++worker_count_;
      ++global_thread_count();
      std::thread thread(&CmtEpoll::loop, this, group_id_, 999, false, -1, true,
                         true);
      thread.detach();
      scaled = true;
    } else if (workers < prefer_thread_count) {
      do {
        ++worker_count_;
        ++global_thread_count();
        std::thread thread(&CmtEpoll::loop, this, group_id_, 999, false, -1,
                           true, true);
        thread.detach();
      } while (worker_count_.load(std::memory_order_acquire) <
               prefer_thread_count);
      scaled = true;
    }

    if (scaled && enable_thread_pool_log)
      my_plugin_log_message(
          &plugin_info.plugin_info, MY_WARNING_LEVEL,
          "MtEpoll %u thread pool scale to worker %d tasker %d. Total threads "
          "%d. wait_type %d",
          group_id_, worker_count_.load(std::memory_order_acquire),
          tasker_count_.load(std::memory_order_acquire),
          global_thread_count().load(std::memory_order_acquire), wait_type);
  }

  inline bool shrink_thread_pool(bool is_worker) {
    if (!is_worker) {
      /// tasker thread
      if (Ctime::steady_ms() -
              last_tasker_time_.load(std::memory_order_acquire) <=
          epoll_group_dynamic_threads_shrink_time)
        return false;

      /// free it
      --tasker_count_;
      --global_thread_count();

      if (enable_thread_pool_log)
        my_plugin_log_message(
            &plugin_info.plugin_info, MY_WARNING_LEVEL,
            "MtEpoll %u thread pool shrink to worker %d tasker %d. Total "
            "thread %d.",
            group_id_, worker_count_.load(std::memory_order_acquire),
            tasker_count_.load(std::memory_order_acquire),
            global_thread_count().load(std::memory_order_acquire));
      return true;
    }

    auto bret = false;
    auto prefer_thread_count =
        static_cast<int>(base_thread_count_ + epoll_group_dynamic_threads);
    auto thresh = static_cast<int>(epoll_group_thread_scale_thresh);
    if (UNLIKELY(thresh < 0))
      thresh = 0;
    else if (UNLIKELY(thresh >= base_thread_count_)) {
      thresh = base_thread_count_ - 1;
      assert(thresh >= 0);
    }
    auto stalled = stall_count_.load(std::memory_order_acquire);
    assert(stalled >= 0);
    auto workers = worker_count_.load(std::memory_order_acquire);
    assert(workers >= 0);

    /// enter mutex only when we need to do
    if (stalled < workers - base_thread_count_ + thresh &&
        Ctime::steady_ms() - last_scale_time_.load(std::memory_order_acquire) >
            epoll_group_dynamic_threads_shrink_time &&
        workers > prefer_thread_count) {
      /// shrink only when no waiting exists and no multiple stall for a while
      std::lock_guard<std::mutex> lck(scale_lock_);
      /// recheck waiting
      stalled = stall_count_.load(std::memory_order_acquire);
      if (worker_count_.load(std::memory_order_acquire) > prefer_thread_count &&
          stalled < prefer_thread_count - 1) {
        --worker_count_;
        --global_thread_count();
        bret = true;

        if (enable_thread_pool_log) {
          my_plugin_log_message(
              &plugin_info.plugin_info, MY_WARNING_LEVEL,
              "MtEpoll %u thread pool shrink to worker %d tasker %d. Total "
              "threads %d.",
              group_id_, worker_count_.load(std::memory_order_acquire),
              tasker_count_.load(std::memory_order_acquire),
              global_thread_count().load(std::memory_order_acquire));
        }
      }
    }
    return bret;
  }
};

} // namespace polarx_rpc
