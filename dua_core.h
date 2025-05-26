// dua_core.h - Core functionality for Disk Usage Analyzer
#ifndef DUA_CORE_H
#define DUA_CORE_H

#include <iostream>
#include <filesystem>
#include <vector>
#include <algorithm>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>
#include <condition_variable>
#include <iomanip>
#include <chrono>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <memory>
#include <future>
#include <numeric>
#include <regex>
#include <cstdlib>
#include <fstream>
#include <set>
#include <unistd.h>
#include <deque>

#ifdef __linux__
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <dirent.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif

#ifdef __APPLE__
#include <sys/stat.h>
#include <sys/mount.h>
#endif

namespace fs = std::filesystem;

// Constants for performance tuning
constexpr size_t THREAD_POOL_SIZE = 0;  // 0 = auto-detect
constexpr size_t BATCH_SIZE = 256;      // Files to process per batch
constexpr size_t QUEUE_SIZE_LIMIT = 50000;
constexpr size_t PREALLOCATE_ENTRIES = 100;
constexpr auto FS_TIMEOUT = std::chrono::seconds(5);

// ANSI color codes
extern const std::string RESET;
extern const std::string RED;
extern const std::string GREEN;
extern const std::string YELLOW;
extern const std::string BLUE;
extern const std::string MAGENTA;
extern const std::string CYAN;
extern const std::string BOLD;
extern const std::string GRAY;

// Progress reporting constants
extern const std::string CLEAR_LINE;

// Configuration structure
struct Config {
    bool interactive_mode = false;
    bool apparent_size = false;
    bool count_hard_links = false;
    bool stay_on_filesystem = false;
    bool no_entry_check = false;
    bool use_trash = false;
    bool no_colors = false;
    bool tree_mode = false;
    bool show_progress = true;
    int max_depth = -1;
    int top_n = -1;
    size_t thread_count = 0;
    std::string format = "metric";
    std::set<fs::path> ignore_dirs;
    std::vector<fs::path> paths;
};

// Entry structure
struct Entry {
    fs::path path;
    std::atomic<uintmax_t> size{0};
    std::atomic<uintmax_t> apparent_size{0};
    bool is_directory{false};
    bool is_symlink{false};
    fs::path symlink_target;
    std::vector<std::shared_ptr<Entry>> children;
    mutable std::mutex children_mutex;
    fs::file_time_type last_modified;
    std::atomic<bool> marked{false};
    std::atomic<uint64_t> entry_count{0};
    dev_t device_id{0};
    ino_t inode{0};
    nlink_t hard_link_count{1};
    
    Entry(const fs::path& p = "");
};

// Progress throttle class
class ProgressThrottle {
private:
    mutable std::chrono::steady_clock::time_point last_update;
    std::chrono::milliseconds update_interval;
    bool is_tty;
    mutable std::mutex mutex;
    
public:
    ProgressThrottle(std::chrono::milliseconds interval = std::chrono::milliseconds(100));
    bool should_update() const;
    void clear_line() const;
};

// Work-stealing thread pool
class WorkStealingThreadPool {
private:
    struct WorkQueue {
        std::deque<std::function<void()>> tasks;
        std::mutex mutex;
        std::atomic<size_t> size{0};
    };
    
    std::vector<std::thread> workers;
    std::vector<std::unique_ptr<WorkQueue>> queues;
    std::condition_variable work_available;
    std::mutex global_mutex;
    std::atomic<bool> stop{false};
    std::atomic<size_t> active_workers{0};
    std::atomic<size_t> total_tasks{0};
    size_t num_threads;
    
    bool try_steal(size_t thief_id, std::function<void()>& task);
    void worker_thread(size_t id);
    
public:
    explicit WorkStealingThreadPool(size_t threads = 0);
    ~WorkStealingThreadPool();
    
    template<class F>
    void enqueue(F&& f);
    
    void wait_all();
};

// Optimized scanner
class OptimizedScanner {
private:
    WorkStealingThreadPool& pool;
    Config& config;
    std::atomic<uintmax_t> total_size{0};
    std::atomic<size_t> file_count{0};
    std::atomic<size_t> dir_count{0};
    std::atomic<size_t> symlink_count{0};
    std::atomic<size_t> io_errors{0};
    std::atomic<size_t> entries_traversed{0};
    std::atomic<size_t> skipped_entries{0};
    std::chrono::steady_clock::time_point start_time;
    ProgressThrottle progress_throttle;
    std::string current_path;
    mutable std::mutex current_path_mutex;
    
    struct InodeKey {
        dev_t device;
        ino_t inode;
        bool operator==(const InodeKey& other) const;
    };
    
    struct InodeKeyHash {
        std::size_t operator()(const InodeKey& k) const;
    };
    
    std::unordered_map<InodeKey, size_t, InodeKeyHash> inode_map;
    std::mutex inode_mutex;
    std::unordered_set<std::string> visited_dirs;
    std::mutex visited_mutex;
    
    bool should_count_entry(const Entry& entry);
    bool should_ignore_directory(const fs::path& path);
    void update_progress() const;
    bool try_iterate_directory(const fs::path& dir_path, 
                              std::vector<fs::directory_entry>& entries);
    void scan_directory_batch(std::shared_ptr<Entry> parent, 
                            const std::vector<fs::directory_entry>& batch,
                            dev_t root_device);
    void scan_directory_impl(std::shared_ptr<Entry> entry, dev_t root_device);
    uintmax_t calculate_sizes(std::shared_ptr<Entry> entry);
    
public:
    OptimizedScanner(WorkStealingThreadPool& tp, Config& cfg);
    std::vector<std::shared_ptr<Entry>> scan(const std::vector<fs::path>& paths);
    void print_stats();
};

// Utility functions
std::string format_size(uintmax_t bytes, const std::string& format);
uintmax_t get_size_on_disk(const fs::path& path, uintmax_t file_size);
bool glob_match(const std::string& pattern, const std::string& text);
std::string shorten_path(const std::string& path, size_t max_length = 45);
void print_tree_sorted(const std::shared_ptr<Entry>& entry, const Config& config,
                      const std::string& prefix = "", bool is_last = true, 
                      int depth = 0, std::vector<std::shared_ptr<Entry>>* siblings = nullptr);

// Template implementation for WorkStealingThreadPool
template<class F>
void WorkStealingThreadPool::enqueue(F&& f) {
    if (stop) return;
    
    static std::atomic<size_t> next_queue{0};
    const size_t actual_threads = queues.size();
    size_t queue_id = next_queue.fetch_add(1) % actual_threads;
    
    size_t attempts = 0;
    while (attempts < actual_threads) {
        auto& queue = queues[queue_id];
        
        if (queue->size.load() < QUEUE_SIZE_LIMIT / actual_threads) {
            std::lock_guard<std::mutex> lock(queue->mutex);
            queue->tasks.emplace_back(std::forward<F>(f));
            queue->size++;
            total_tasks++;
            work_available.notify_one();
            return;
        }
        
        queue_id = (queue_id + 1) % actual_threads;
        attempts++;
    }
    
    f();
}

#endif // DUA_CORE_H
