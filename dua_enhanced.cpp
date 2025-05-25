// dua_enhanced_optimized.cpp - Enhanced Disk Usage Analyzer with optimized UI performance
// Complete reimplementation of dua-cli with additional features and robustness improvements
// Version 1.1.0 - Added symlink display support

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
#include <ncurses.h>
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
#include <unistd.h>  // For isatty()
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
constexpr size_t QUEUE_SIZE_LIMIT = 50000;  // Increased to reduce blocking
constexpr size_t PREALLOCATE_ENTRIES = 100;
constexpr auto FS_TIMEOUT = std::chrono::seconds(5);  // Timeout for filesystem operations

// ANSI color codes
const std::string RESET = "\033[0m";
const std::string RED = "\033[31m";
const std::string GREEN = "\033[32m";
const std::string YELLOW = "\033[33m";
const std::string BLUE = "\033[34m";
const std::string MAGENTA = "\033[35m";  // Added for symlinks
const std::string CYAN = "\033[36m";
const std::string BOLD = "\033[1m";
const std::string GRAY = "\033[90m";

// Progress reporting constants
const std::string CLEAR_LINE = "\033[2K\r";

// Forward declarations
class WorkStealingThreadPool;
class OptimizedScanner;
class InteractiveUI;
struct Entry;
struct Config;

// Function declarations with default parameters
void print_tree_sorted(const std::shared_ptr<Entry>& entry, const Config& config,
                      const std::string& prefix = "", bool is_last = true, 
                      int depth = 0, std::vector<std::shared_ptr<Entry>>* siblings = nullptr);
void aggregate_mode(Config& config);

// Configuration structure
struct Config {
    bool interactive_mode = false;
    bool apparent_size = false;
    bool count_hard_links = false;
    bool stay_on_filesystem = false;
    bool no_entry_check = false;
    bool use_trash = false;
    bool no_colors = false;  // Default to colors enabled
    bool tree_mode = false;  // Display as tree instead of flat list
    bool show_progress = true;  // Show progress during scan
    int max_depth = -1;
    int top_n = -1;
    size_t thread_count = 0;
    std::string format = "metric";  // metric, binary, bytes, gb, gib, mb, mib
    std::set<fs::path> ignore_dirs;
    std::vector<fs::path> paths;
};

// Progress throttle class
class ProgressThrottle {
private:
    mutable std::chrono::steady_clock::time_point last_update;
    std::chrono::milliseconds update_interval;
    bool is_tty;
    mutable std::mutex mutex;
    
public:
    ProgressThrottle(std::chrono::milliseconds interval = std::chrono::milliseconds(100)) 
        : update_interval(interval) {
        is_tty = isatty(fileno(stderr));
        last_update = std::chrono::steady_clock::now();
    }
    
    bool should_update() const {
        if (!is_tty) return false;
        
        std::lock_guard<std::mutex> lock(mutex);
        auto now = std::chrono::steady_clock::now();
        if (now - last_update >= update_interval) {
            last_update = now;
            return true;
        }
        return false;
    }
    
    void clear_line() const {
        if (is_tty) {
            std::cerr << CLEAR_LINE << std::flush;
        }
    }
};

// Helper function to shorten paths for display
// Preserves first 30 and last 30 characters for long paths
std::string shorten_path(const std::string& path, size_t max_length = 45) {
    if (path.length() <= max_length) {
        return path;
    }
    
    // For paths longer than max_length, show first 30 and last 30 chars
    const size_t prefix_len = 30;
    const size_t suffix_len = 30;
    const std::string ellipsis = "...";
    
    // Make sure we have enough characters
    if (path.length() <= prefix_len + suffix_len + ellipsis.length()) {
        return path;
    }
    
    return path.substr(0, prefix_len) + ellipsis + 
           path.substr(path.length() - suffix_len);
}

// Enhanced entry structure with symlink support
struct Entry {
    fs::path path;
    std::atomic<uintmax_t> size{0};
    std::atomic<uintmax_t> apparent_size{0};
    bool is_directory{false};
    bool is_symlink{false};
    fs::path symlink_target;  // Added to store where the symlink points
    std::vector<std::shared_ptr<Entry>> children;
    mutable std::mutex children_mutex;
    fs::file_time_type last_modified;
    std::atomic<bool> marked{false};
    std::atomic<uint64_t> entry_count{0};
    dev_t device_id{0};
    ino_t inode{0};
    nlink_t hard_link_count{1};
    
    Entry(const fs::path& p = "") : path(p) {
        children.reserve(PREALLOCATE_ENTRIES);
        try {
            if (fs::exists(path)) {
                auto status = fs::symlink_status(path);
                is_symlink = fs::is_symlink(status);
                
                if (is_symlink) {
                    // Get the symlink target - this is crucial for display
                    try {
                        symlink_target = fs::read_symlink(path);
                    } catch (...) {
                        // Handle broken symlinks gracefully
                        symlink_target = fs::path("[unreadable]");
                    }
                    // Symlinks don't have meaningful modification times
                    last_modified = fs::file_time_type{};
                } else {
                    // Regular files and directories
                    last_modified = fs::last_write_time(path);
                    
                    // Get inode information for regular files/dirs
#ifdef __linux__
                    struct stat st;
                    if (stat(path.c_str(), &st) == 0) {
                        device_id = st.st_dev;
                        inode = st.st_ino;
                        hard_link_count = st.st_nlink;
                    }
#endif
                }
            }
        } catch (...) {
            last_modified = fs::file_time_type{};
        }
    }
};

// Format size based on configuration
std::string format_size(uintmax_t bytes, const std::string& format) {
    if (format == "bytes") {
        return std::to_string(bytes) + " B";
    }
    
    double size = static_cast<double>(bytes);
    int unit_index = 0;
    
    if (format == "metric") {
        const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
        double divisor = 1000.0;
        
        while (size >= divisor && unit_index < 5) {
            size /= divisor;
            unit_index++;
        }
        
        std::ostringstream oss;
        if (unit_index == 0) {
            oss << static_cast<int>(size) << " " << units[unit_index];
        } else {
            oss << std::fixed << std::setprecision(2) << size << " " << units[unit_index];
        }
        return oss.str();
    } else if (format == "binary") {
        const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
        double divisor = 1024.0;
        
        while (size >= divisor && unit_index < 5) {
            size /= divisor;
            unit_index++;
        }
        
        std::ostringstream oss;
        if (unit_index == 0) {
            oss << static_cast<int>(size) << " " << units[unit_index];
        } else {
            oss << std::fixed << std::setprecision(2) << size << " " << units[unit_index];
        }
        return oss.str();
    } else if (format == "gb") {
        size /= 1000000000.0;
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << size << " GB";
        return oss.str();
    } else if (format == "gib") {
        size /= 1073741824.0;
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << size << " GiB";
        return oss.str();
    } else if (format == "mb") {
        size /= 1000000.0;
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << size << " MB";
        return oss.str();
    } else if (format == "mib") {
        size /= 1048576.0;
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << size << " MiB";
        return oss.str();
    }
    
    return std::to_string(bytes) + " B";
}

// Get size on disk (block-aligned)
uintmax_t get_size_on_disk([[maybe_unused]] const fs::path& path, uintmax_t file_size) {
#ifdef __linux__
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        // Size on disk is st_blocks * 512 (block size)
        return st.st_blocks * 512;
    }
#endif
    // Fallback: round up to nearest 4KB block
    const uintmax_t block_size = 4096;
    return ((file_size + block_size - 1) / block_size) * block_size;
}

// Glob pattern matching
bool glob_match(const std::string& pattern, const std::string& text) {
    // Convert glob pattern to regex
    std::string regex_pattern;
    for (char c : pattern) {
        switch (c) {
            case '*':
                regex_pattern += ".*";
                break;
            case '?':
                regex_pattern += ".";
                break;
            case '.':
            case '(':
            case ')':
            case '[':
            case ']':
            case '{':
            case '}':
            case '+':
            case '^':
            case '$':
            case '|':
            case '\\':
                regex_pattern += "\\";
                regex_pattern += c;
                break;
            default:
                regex_pattern += c;
        }
    }
    
    try {
        std::regex re(regex_pattern, std::regex_constants::icase);
        return std::regex_search(text, re);
    } catch (...) {
        return false;
    }
}

// Work-stealing thread pool to prevent deadlocks
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
    size_t num_threads;  // Changed from const to mutable
    
    // Steal work from other queues
    bool try_steal(size_t thief_id, std::function<void()>& task) {
        // Try to steal from other queues in round-robin fashion
        const size_t actual_threads = queues.size();  // Use actual queue count
        for (size_t i = 1; i < actual_threads; ++i) {
            size_t victim_id = (thief_id + i) % actual_threads;
            auto& victim_queue = queues[victim_id];
            
            if (victim_queue->size.load() > 0) {
                std::unique_lock<std::mutex> lock(victim_queue->mutex, std::try_to_lock);
                if (lock.owns_lock() && !victim_queue->tasks.empty()) {
                    // Steal from the back (FIFO for stolen tasks)
                    task = std::move(victim_queue->tasks.back());
                    victim_queue->tasks.pop_back();
                    victim_queue->size--;
                    return true;
                }
            }
        }
        return false;
    }
    
    void worker_thread(size_t id) {
        auto& my_queue = queues[id];
        
        while (!stop) {
            std::function<void()> task;
            
            // First, try to get work from own queue
            {
                std::unique_lock<std::mutex> lock(my_queue->mutex);
                if (!my_queue->tasks.empty()) {
                    task = std::move(my_queue->tasks.front());
                    my_queue->tasks.pop_front();
                    my_queue->size--;
                }
            }
            
            // If no work in own queue, try to steal
            if (!task && !try_steal(id, task)) {
                // No work available anywhere, wait
                std::unique_lock<std::mutex> lock(global_mutex);
                work_available.wait_for(lock, std::chrono::milliseconds(10),
                    [this] { return stop.load() || total_tasks.load() > 0; });
                continue;
            }
            
            if (task) {
                active_workers++;
                task();
                active_workers--;
                total_tasks--;
            }
        }
    }
    
public:
    explicit WorkStealingThreadPool(size_t threads = 0) {
        // Determine the actual number of threads to use
        num_threads = threads;
        if (num_threads == 0) {
            num_threads = std::thread::hardware_concurrency();
            if (num_threads == 0) num_threads = 4;
        }
        
        // Special handling for macOS - use 3 threads for better performance
#ifdef __APPLE__
        num_threads = std::min(num_threads, size_t(3));
#endif
        
        // Important: Ensure num_threads matches the actual number of queues
        // This prevents out-of-bounds access
        
        // Create per-thread queues
        queues.reserve(num_threads);
        for (size_t i = 0; i < num_threads; ++i) {
            queues.emplace_back(std::make_unique<WorkQueue>());
        }
        
        // Start worker threads
        workers.reserve(num_threads);
        for (size_t i = 0; i < num_threads; ++i) {
            workers.emplace_back(&WorkStealingThreadPool::worker_thread, this, i);
        }
    }
    
    ~WorkStealingThreadPool() {
        stop = true;
        work_available.notify_all();
        for (auto& worker : workers) {
            worker.join();
        }
    }
    
    template<class F>
    void enqueue(F&& f) {
        if (stop) return;
        
        // Use round-robin to distribute tasks
        static std::atomic<size_t> next_queue{0};
        const size_t actual_threads = queues.size();  // Always use actual queue count
        size_t queue_id = next_queue.fetch_add(1) % actual_threads;
        
        // Try to find a queue with space
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
        
        // All queues are full - execute directly to prevent deadlock
        f();
    }
    
    void wait_all() {
        while (total_tasks.load() > 0 || active_workers.load() > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
};

// Enhanced scanner with timeout protection and symlink handling
class OptimizedScanner {
private:
    WorkStealingThreadPool& pool;
    Config& config;
    std::atomic<uintmax_t> total_size{0};
    std::atomic<size_t> file_count{0};
    std::atomic<size_t> dir_count{0};
    std::atomic<size_t> symlink_count{0};  // Added counter for symlinks
    std::atomic<size_t> io_errors{0};
    std::atomic<size_t> entries_traversed{0};
    std::atomic<size_t> skipped_entries{0};
    std::chrono::steady_clock::time_point start_time;
    ProgressThrottle progress_throttle;
    std::string current_path;
    mutable std::mutex current_path_mutex;
    
    // Hard link tracking
    struct InodeKey {
        dev_t device;
        ino_t inode;
        
        bool operator==(const InodeKey& other) const {
            return device == other.device && inode == other.inode;
        }
    };
    
    struct InodeKeyHash {
        std::size_t operator()(const InodeKey& k) const {
            return std::hash<dev_t>()(k.device) ^ (std::hash<ino_t>()(k.inode) << 1);
        }
    };
    
    std::unordered_map<InodeKey, size_t, InodeKeyHash> inode_map;
    std::mutex inode_mutex;
    
    // Directory cache to prevent revisiting
    std::unordered_set<std::string> visited_dirs;
    std::mutex visited_mutex;
    
    bool should_count_entry(const Entry& entry) {
        if (!config.count_hard_links && entry.hard_link_count > 1) {
            std::lock_guard<std::mutex> lock(inode_mutex);
            InodeKey key{entry.device_id, entry.inode};
            auto it = inode_map.find(key);
            if (it != inode_map.end()) {
                // Already counted this hard link
                return false;
            }
            inode_map[key] = 1;
        }
        return true;
    }
    
    bool should_ignore_directory(const fs::path& path) {
        fs::path canonical_path;
        try {
            canonical_path = fs::canonical(path);
        } catch (...) {
            canonical_path = path;
        }
        
        // Check if already visited (prevents symlink loops)
        {
            std::lock_guard<std::mutex> lock(visited_mutex);
            if (!visited_dirs.insert(canonical_path.string()).second) {
                return true;  // Already visited
            }
        }
        
        return config.ignore_dirs.find(canonical_path) != config.ignore_dirs.end();
    }
    
    void update_progress() const {
        if (config.show_progress && progress_throttle.should_update()) {
            size_t current_entries = entries_traversed.load();
            size_t skipped = skipped_entries.load();
            std::string path_display;
            
            {
                std::lock_guard<std::mutex> lock(current_path_mutex);
                path_display = current_path;
            }
            
            // Format the progress message with shortened path
            std::string shortened = shorten_path(path_display);
            std::cerr << "\rEnumerating " << current_entries << " items";
            if (skipped > 0) {
                std::cerr << " (skipped " << skipped << ")";
            }
            std::cerr << " - " << shortened << std::flush;
        }
    }
    
    // Non-blocking directory iteration with timeout
    bool try_iterate_directory(const fs::path& dir_path, 
                              std::vector<fs::directory_entry>& entries) {
        try {
            // Use a future for timeout control
            auto future = std::async(std::launch::async, [&]() {
                try {
                    for (const auto& entry : fs::directory_iterator(dir_path,
                            fs::directory_options::skip_permission_denied)) {
                        entries.push_back(entry);
                    }
                    return true;
                } catch (...) {
                    return false;
                }
            });
            
            // Wait with timeout
            if (future.wait_for(FS_TIMEOUT) == std::future_status::ready) {
                return future.get();
            } else {
                // Timeout occurred
                skipped_entries++;
                return false;
            }
        } catch (...) {
            return false;
        }
    }
    
    void scan_directory_batch(std::shared_ptr<Entry> parent, 
                            const std::vector<fs::directory_entry>& batch,
                            dev_t root_device) {
        for (const auto& item : batch) {
            try {
                auto child = std::make_shared<Entry>(item.path());
                
                // Update current path for progress display
                {
                    std::lock_guard<std::mutex> lock(current_path_mutex);
                    current_path = item.path().string();
                }
                
                // Check filesystem boundary (but not for symlinks)
                if (!child->is_symlink && config.stay_on_filesystem && 
                    child->device_id != root_device) {
                    continue;
                }
                
                entries_traversed++;
                update_progress();
                
                if (child->is_symlink) {
                    // Handle symlinks - add them to the tree but don't follow
                    symlink_count++;  // Track symlink count
                    
                    // Symlinks have no meaningful size - this is important
                    // because the actual symlink file is tiny (just stores a path)
                    child->size = 0;
                    child->apparent_size = 0;
                    child->entry_count = 0;
                    
                    {
                        std::lock_guard<std::mutex> lock(parent->children_mutex);
                        parent->children.push_back(child);
                    }
                } else if (item.is_directory()) {
                    child->is_directory = true;
                    dir_count++;
                    
                    {
                        std::lock_guard<std::mutex> lock(parent->children_mutex);
                        parent->children.push_back(child);
                    }
                    
                    // Schedule directory scanning
                    pool.enqueue([this, child, root_device]() {
                        scan_directory_impl(child, root_device);
                    });
                } else if (item.is_regular_file()) {
                    child->apparent_size = item.file_size();
                    
                    if (should_count_entry(*child)) {
                        if (config.apparent_size) {
                            child->size = child->apparent_size.load();
                        } else {
                            child->size = get_size_on_disk(child->path, child->apparent_size);
                        }
                        file_count++;
                        parent->entry_count++;
                    }
                    
                    {
                        std::lock_guard<std::mutex> lock(parent->children_mutex);
                        parent->children.push_back(child);
                    }
                    
                    parent->size += child->size.load();
                }
            } catch (const fs::filesystem_error&) {
                io_errors++;
            }
        }
    }
    
    void scan_directory_impl(std::shared_ptr<Entry> entry, dev_t root_device) {
        if (entry->is_symlink || should_ignore_directory(entry->path)) {
            return;
        }
        
        // Update current path for progress display
        {
            std::lock_guard<std::mutex> lock(current_path_mutex);
            current_path = entry->path.string();
        }
        
        std::vector<fs::directory_entry> entries;
        entries.reserve(BATCH_SIZE * 2);
        
        // Try to iterate directory with timeout
        if (!try_iterate_directory(entry->path, entries)) {
            io_errors++;
            return;
        }
        
        // Process entries in batches to improve parallelism
        std::vector<fs::directory_entry> batch;
        batch.reserve(BATCH_SIZE);
        
        for (const auto& item : entries) {
            batch.push_back(item);
            
            if (batch.size() >= BATCH_SIZE) {
                scan_directory_batch(entry, batch, root_device);
                batch.clear();
            }
        }
        
        // Process remaining items
        if (!batch.empty()) {
            scan_directory_batch(entry, batch, root_device);
        }
    }
    
    uintmax_t calculate_sizes(std::shared_ptr<Entry> entry) {
        if (!entry->is_directory) {
            entry->entry_count = entry->size > 0 ? 1 : 0;
            return entry->size.load();
        }
        
        uintmax_t total = 0;
        uint64_t count = 0;
        
        {
            std::lock_guard<std::mutex> lock(entry->children_mutex);
            for (auto& child : entry->children) {
                total += calculate_sizes(child);
                count += child->entry_count.load();
            }
            
            std::sort(entry->children.begin(), entry->children.end(),
                [](const std::shared_ptr<Entry>& a, const std::shared_ptr<Entry>& b) {
                    return a->size.load() > b->size.load();
                });
        }
        
        entry->size = total;
        entry->entry_count = count;
        return total;
    }
    
public:
    OptimizedScanner(WorkStealingThreadPool& tp, Config& cfg) 
        : pool(tp), config(cfg), progress_throttle(std::chrono::milliseconds(100)) {
        start_time = std::chrono::steady_clock::now();
    }
    
    std::vector<std::shared_ptr<Entry>> scan(const std::vector<fs::path>& paths) {
        std::vector<std::shared_ptr<Entry>> roots;
        
        for (const auto& path : paths) {
            auto root = std::make_shared<Entry>(path);
            root->is_directory = fs::is_directory(path);
            
            // Set initial path for progress display
            {
                std::lock_guard<std::mutex> lock(current_path_mutex);
                current_path = path.string();
            }
            
            if (root->is_directory) {
                dir_count++;
                entries_traversed++;
                update_progress();
                scan_directory_impl(root, root->device_id);
            } else {
                root->apparent_size = fs::file_size(path);
                root->size = config.apparent_size ? root->apparent_size.load() : 
                           get_size_on_disk(path, root->apparent_size);
                file_count++;
                entries_traversed++;
                update_progress();
            }
            
            roots.push_back(root);
        }
        
        pool.wait_all();
        
        // Clear progress line
        if (config.show_progress) {
            progress_throttle.clear_line();
        }
        
        for (auto& root : roots) {
            total_size += calculate_sizes(root);
        }
        
        return roots;
    }
    
    void print_stats() {
        auto duration = std::chrono::steady_clock::now() - start_time;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
        
        std::cerr << "\nScanned " << file_count << " files, " 
                  << dir_count << " directories, and " 
                  << symlink_count << " symlinks in " << ms << "ms\n";
        if (io_errors > 0) {
            std::cerr << "Encountered " << io_errors << " I/O errors\n";
        }
        if (skipped_entries > 0) {
            std::cerr << "Skipped " << skipped_entries << " unresponsive directories\n";
        }
        std::cerr << "Total size: " << format_size(total_size, config.format) << "\n";
    }
};

// Cache for rendered lines to detect changes
struct LineCache {
    std::string content;
    int attributes;
    bool is_selected;
    
    bool operator!=(const LineCache& other) const {
        return content != other.content || 
               attributes != other.attributes || 
               is_selected != other.is_selected;
    }
};

// Interactive UI with enhanced features including symlink display
class InteractiveUI {
private:
    std::vector<std::shared_ptr<Entry>> roots;
    std::vector<std::shared_ptr<Entry>> current_view;
    std::shared_ptr<Entry> current_dir;
    size_t selected_index = 0;
    size_t view_offset = 0;
    bool show_help = false;
    bool show_mtime = false;
    bool show_count = false;
    bool glob_search_active = false;
    std::string glob_pattern;
    std::vector<std::shared_ptr<Entry>> navigation_stack;
    Config& config;
    
    // Performance optimization: cache rendered lines
    std::vector<LineCache> line_cache;
    bool needs_full_redraw = true;
    size_t last_selected_index = SIZE_MAX;
    size_t last_view_offset = SIZE_MAX;
    std::string last_status_line;
    std::string last_path_line;
    
    // Event batching for smooth movement
    std::chrono::steady_clock::time_point last_input_time;
    static constexpr auto INPUT_BATCH_DELAY = std::chrono::milliseconds(5);
    
    // Cached formatted strings
    struct CachedEntry {
        std::string formatted_size;
        std::string formatted_time;
        std::string formatted_name;
        double percentage;
        bool needs_update = true;
    };
    std::unordered_map<std::shared_ptr<Entry>, CachedEntry> format_cache;
    
    enum class SortMode {
        SIZE_DESC,
        SIZE_ASC,
        NAME_ASC,
        NAME_DESC,
        TIME_DESC,
        TIME_ASC,
        COUNT_DESC,
        COUNT_ASC
    };
    SortMode sort_mode = SortMode::SIZE_DESC;
    
public:
    InteractiveUI(std::vector<std::shared_ptr<Entry>> root_entries, Config& cfg) 
        : roots(root_entries), config(cfg) {
        
        // Create virtual root if multiple paths
        if (roots.size() > 1) {
            auto virtual_root = std::make_shared<Entry>("");
            virtual_root->is_directory = true;
            for (auto& root : roots) {
                virtual_root->children.push_back(root);
                virtual_root->size += root->size.load();
                virtual_root->entry_count += root->entry_count.load();
            }
            current_dir = virtual_root;
        } else {
            current_dir = roots[0];
        }
        
        update_view();
        navigation_stack.push_back(current_dir);
        
        // Pre-allocate cache
        line_cache.reserve(LINES);
    }
    
    void run() {
        // Initialize ncurses with optimizations
        initscr();
        cbreak();
        noecho();
        keypad(stdscr, TRUE);
        curs_set(0);
        
        // Enable immediate key response
        nodelay(stdscr, TRUE);  // Non-blocking getch()
        
        // Optimize screen updates
        scrollok(stdscr, FALSE);  // Disable scrolling
        idlok(stdscr, TRUE);      // Use hardware insert/delete line
        idcok(stdscr, TRUE);      // Use hardware insert/delete char
        
        if (has_colors()) {
            start_color();
            init_pair(1, COLOR_CYAN, COLOR_BLACK);    // Directories
            init_pair(2, COLOR_WHITE, COLOR_BLACK);   // Files
            init_pair(3, COLOR_GREEN, COLOR_BLACK);   // Size
            init_pair(4, COLOR_BLACK, COLOR_CYAN);    // Selected
            init_pair(5, COLOR_WHITE, COLOR_BLACK);   // Header
            init_pair(6, COLOR_YELLOW, COLOR_BLACK);  // Percentage
            init_pair(7, COLOR_BLUE, COLOR_BLACK);    // Help
            init_pair(8, COLOR_RED, COLOR_BLACK);     // Marked
            init_pair(9, COLOR_MAGENTA, COLOR_BLACK); // Symlinks
        }
        
        bool running = true;
        int pending_move = 0;  // Accumulate rapid movements
        
        while (running) {
            // Draw only what's needed
            if (needs_full_redraw) {
                draw_full();
                needs_full_redraw = false;
            } else {
                draw_differential();
            }
            
            // Handle input with batching for smooth movement
            int ch = getch();
            if (ch != ERR) {
                auto now = std::chrono::steady_clock::now();
                bool is_movement = (ch == KEY_UP || ch == KEY_DOWN || 
                                   ch == 'j' || ch == 'k');
                
                if (is_movement) {
                    // Batch rapid movements
                    if (now - last_input_time < INPUT_BATCH_DELAY) {
                        pending_move += (ch == KEY_DOWN || ch == 'j') ? 1 : -1;
                        napms(1);  // Brief pause to collect more input
                        continue;
                    }
                    
                    // Apply accumulated movement
                    if (pending_move != 0) {
                        apply_movement(pending_move);
                        pending_move = 0;
                    }
                    
                    // Apply current movement
                    if (ch == KEY_UP || ch == 'k') {
                        navigate_up();
                    } else {
                        navigate_down();
                    }
                    
                    last_input_time = now;
                } else {
                    // Handle non-movement keys
                    if (glob_search_active) {
                        handle_glob_search(ch);
                    } else {
                        if (handle_key(ch) == false) {
                            running = false;
                        }
                    }
                }
            } else {
                // No input - apply any pending movement
                if (pending_move != 0) {
                    apply_movement(pending_move);
                    pending_move = 0;
                }
                napms(10);  // Small delay when idle
            }
        }
        
        endwin();
        
        // Print marked paths on exit
        print_marked_paths();
    }
    
private:
    // Apply accumulated movement efficiently
    void apply_movement(int delta) {
        if (delta == 0) return;
        
        size_t new_index = selected_index;
        if (delta > 0) {
            new_index = std::min(selected_index + delta, current_view.size() - 1);
        } else {
            size_t abs_delta = static_cast<size_t>(-delta);
            new_index = (abs_delta > selected_index) ? 0 : selected_index - abs_delta;
        }
        
        if (new_index != selected_index) {
            selected_index = new_index;
            
            // Adjust view offset if needed
            int max_visible = LINES - 4;
            if (static_cast<int>(selected_index) < static_cast<int>(view_offset)) {
                view_offset = selected_index;
            } else if (static_cast<int>(selected_index) >= static_cast<int>(view_offset) + max_visible) {
                view_offset = selected_index - max_visible + 1;
            }
        }
    }
    
    void navigate_up() {
        if (selected_index > 0) {
            selected_index--;
            if (selected_index < view_offset) {
                view_offset = selected_index;
            }
        }
    }
    
    void navigate_down() {
        if (selected_index + 1 < current_view.size()) {
            selected_index++;
            int max_visible = LINES - 4;
            if (static_cast<int>(selected_index) >= static_cast<int>(view_offset) + max_visible) {
                view_offset = selected_index - max_visible + 1;
            }
        }
    }
    
    void enter_directory() {
        if (selected_index < current_view.size()) {
            auto selected = current_view[selected_index];
            // Can only enter directories, not symlinks
            if (selected->is_directory && !selected->is_symlink && !selected->children.empty()) {
                current_dir = selected;
                navigation_stack.push_back(current_dir);
                update_view();
                selected_index = 0;
                view_offset = 0;
                needs_full_redraw = true;
            }
        }
    }
    
    void exit_directory() {
        if (navigation_stack.size() > 1) {
            navigation_stack.pop_back();
            current_dir = navigation_stack.back();
            update_view();
            selected_index = 0;
            view_offset = 0;
            needs_full_redraw = true;
        }
    }
    
    void open_selected() {
        if (selected_index < current_view.size()) {
            auto selected = current_view[selected_index];
            std::string command;
            
#ifdef __linux__
            command = "xdg-open ";
#elif __APPLE__
            command = "open ";
#else
            return;  // Not supported
#endif
            
            command += "\"" + selected->path.string() + "\" 2>/dev/null &";
            system(command.c_str());
        }
    }
    
    void start_glob_search() {
        glob_search_active = true;
        glob_pattern.clear();
    }
    
    void handle_glob_search(int ch) {
        // Show search prompt
        move(LINES - 1, 0);
        clrtoeol();
        mvprintw(LINES - 1, 0, "Search: %s", glob_pattern.c_str());
        refresh();
        
        if (ch == 27) {  // ESC
            glob_search_active = false;
            needs_full_redraw = true;
        } else if (ch == '\n') {
            perform_glob_search();
            glob_search_active = false;
            needs_full_redraw = true;
        } else if (ch == KEY_BACKSPACE || ch == 127) {
            if (!glob_pattern.empty()) {
                glob_pattern.pop_back();
            }
        } else if (ch >= 32 && ch < 127) {
            glob_pattern += static_cast<char>(ch);
        }
    }
    
    void perform_glob_search() {
        if (glob_pattern.empty()) return;
        
        std::vector<std::shared_ptr<Entry>> matches;
        search_entries(current_dir, glob_pattern, matches);
        
        if (!matches.empty()) {
            // Create virtual directory with search results
            auto search_results = std::make_shared<Entry>("[Search Results]");
            search_results->is_directory = true;
            search_results->children = matches;
            
            // Calculate total size
            for (auto& match : matches) {
                search_results->size += match->size.load();
                search_results->entry_count += match->entry_count.load();
            }
            
            current_dir = search_results;
            navigation_stack.push_back(current_dir);
            update_view();
            selected_index = 0;
            view_offset = 0;
        }
    }
    
    void search_entries(std::shared_ptr<Entry> root, const std::string& pattern, 
                       std::vector<std::shared_ptr<Entry>>& matches) {
        if (glob_match(pattern, root->path.filename().string())) {
            matches.push_back(root);
        }
        
        if (root->is_directory && !root->is_symlink) {
            std::lock_guard<std::mutex> lock(root->children_mutex);
            for (auto& child : root->children) {
                search_entries(child, pattern, matches);
            }
        }
    }
    
    void refresh_selected() {
        if (selected_index < current_view.size()) {
            auto selected = current_view[selected_index];
            if (selected->is_directory && !selected->is_symlink) {
                // Show progress
                clear();
                mvprintw(LINES / 2, COLS / 2 - 10, "Refreshing...");
                refresh();
                
                // Re-scan directory
                WorkStealingThreadPool pool(config.thread_count);
                OptimizedScanner scanner(pool, config);
                
                // Clear existing children
                {
                    std::lock_guard<std::mutex> lock(selected->children_mutex);
                    selected->children.clear();
                }
                
                // Re-scan
                auto new_entries = scanner.scan({selected->path});
                if (!new_entries.empty()) {
                    selected->children = new_entries[0]->children;
                    selected->size = new_entries[0]->size.load();
                    selected->entry_count = new_entries[0]->entry_count.load();
                }
                
                update_view();
            }
        }
    }
    
    void refresh_all() {
        // Show progress
        clear();
        mvprintw(LINES / 2, COLS / 2 - 10, "Refreshing all...");
        refresh();
        
        WorkStealingThreadPool pool(config.thread_count);
        OptimizedScanner scanner(pool, config);
        
        if (roots.size() > 1) {
            // Multiple roots - refresh all
            roots = scanner.scan(config.paths);
            
            auto virtual_root = std::make_shared<Entry>("");
            virtual_root->is_directory = true;
            for (auto& root : roots) {
                virtual_root->children.push_back(root);
                virtual_root->size += root->size.load();
                virtual_root->entry_count += root->entry_count.load();
            }
            
            navigation_stack.clear();
            current_dir = virtual_root;
            navigation_stack.push_back(current_dir);
        } else {
            // Single root - refresh it
            roots = scanner.scan({roots[0]->path});
            navigation_stack.clear();
            current_dir = roots[0];
            navigation_stack.push_back(current_dir);
        }
        
        update_view();
        selected_index = 0;
        view_offset = 0;
    }
    
    void update_view() {
        format_cache.clear();
        current_view.clear();
        {
            std::lock_guard<std::mutex> lock(current_dir->children_mutex);
            current_view = current_dir->children;
        }
        apply_sort();
    }
    
    void apply_sort() {
        switch (sort_mode) {
            case SortMode::SIZE_DESC:
                std::sort(current_view.begin(), current_view.end(),
                    [](const std::shared_ptr<Entry>& a, const std::shared_ptr<Entry>& b) { 
                        return a->size.load() > b->size.load(); 
                    });
                break;
            case SortMode::SIZE_ASC:
                std::sort(current_view.begin(), current_view.end(),
                    [](const std::shared_ptr<Entry>& a, const std::shared_ptr<Entry>& b) { 
                        return a->size.load() < b->size.load(); 
                    });
                break;
            case SortMode::NAME_ASC:
                std::sort(current_view.begin(), current_view.end(),
                    [](const std::shared_ptr<Entry>& a, const std::shared_ptr<Entry>& b) { 
                        return a->path.filename() < b->path.filename(); 
                    });
                break;
            case SortMode::NAME_DESC:
                std::sort(current_view.begin(), current_view.end(),
                    [](const std::shared_ptr<Entry>& a, const std::shared_ptr<Entry>& b) { 
                        return a->path.filename() > b->path.filename(); 
                    });
                break;
            case SortMode::TIME_DESC:
                std::sort(current_view.begin(), current_view.end(),
                    [](const std::shared_ptr<Entry>& a, const std::shared_ptr<Entry>& b) { 
                        return a->last_modified > b->last_modified; 
                    });
                break;
            case SortMode::TIME_ASC:
                std::sort(current_view.begin(), current_view.end(),
                    [](const std::shared_ptr<Entry>& a, const std::shared_ptr<Entry>& b) { 
                        return a->last_modified < b->last_modified; 
                    });
                break;
            case SortMode::COUNT_DESC:
                std::sort(current_view.begin(), current_view.end(),
                    [](const std::shared_ptr<Entry>& a, const std::shared_ptr<Entry>& b) { 
                        return a->entry_count.load() > b->entry_count.load(); 
                    });
                break;
            case SortMode::COUNT_ASC:
                std::sort(current_view.begin(), current_view.end(),
                    [](const std::shared_ptr<Entry>& a, const std::shared_ptr<Entry>& b) { 
                        return a->entry_count.load() < b->entry_count.load(); 
                    });
                break;
        }
    }
    
    // Differential rendering - only update changed lines
    void draw_differential() {
        bool selection_changed = (selected_index != last_selected_index);
        bool view_scrolled = (view_offset != last_view_offset);
        
        if (!selection_changed && !view_scrolled) {
            return;  // Nothing to update
        }
        
        int y = 2;
        int max_y = LINES - 2;
        
        // If view scrolled, we need to redraw all visible lines
        if (view_scrolled) {
            for (size_t i = view_offset; i < current_view.size() && y < max_y; i++) {
                draw_entry_line(i, y, true);
                y++;
            }
            
            // Clear any remaining lines
            while (y < max_y) {
                move(y, 0);
                clrtoeol();
                y++;
            }
        } else if (selection_changed) {
            // Only update the old and new selected lines
            if (last_selected_index != SIZE_MAX && 
                last_selected_index >= view_offset && 
                last_selected_index < view_offset + (max_y - 2)) {
                int old_y = 2 + (last_selected_index - view_offset);
                draw_entry_line(last_selected_index, old_y, false);
            }
            
            if (selected_index >= view_offset && 
                selected_index < view_offset + (max_y - 2)) {
                int new_y = 2 + (selected_index - view_offset);
                draw_entry_line(selected_index, new_y, false);
            }
        }
        
        // Update status line if needed
        update_status_line();
        
        // Remember current state
        last_selected_index = selected_index;
        last_view_offset = view_offset;
        
        refresh();
    }
    
    // Draw a single entry line with full-line highlighting
    void draw_entry_line(size_t index, int y, bool force_redraw) {
        if (index >= current_view.size()) return;
        
        auto entry = current_view[index];
        bool is_selected = (index == selected_index);
        
        // Get cached formatting or create new
        auto& cached = format_cache[entry];
        if (cached.needs_update) {
            update_format_cache(entry, cached);
        }
        
        // Check if we need to redraw this line
        if (!force_redraw && index < line_cache.size() && 
            line_cache[index].is_selected == is_selected) {
            return;  // Line hasn't changed
        }
        
        // Move to line position
        move(y, 0);
        
        // Clear the entire line first
        clrtoeol();
        
        // Apply selection highlighting to the entire line
        if (is_selected) {
            attron(COLOR_PAIR(4));
            // Fill the entire line with spaces to create full-line highlight
            for (int i = 0; i < COLS; i++) {
                mvaddch(y, i, ' ');
            }
        }
        
        // Draw the line content
        int col_x = 0;
        
        // Mark indicator
        if (entry->marked.load()) {
            if (!is_selected) attron(COLOR_PAIR(8) | A_BOLD);
            mvaddch(y, col_x, '*');
            if (!is_selected) attroff(COLOR_PAIR(8) | A_BOLD);
        } else {
            mvaddch(y, col_x, ' ');
        }
        col_x = 1;
        
        // Size
        if (is_selected) {
            attron(COLOR_PAIR(4));  // Keep selection color
        } else {
            attron(COLOR_PAIR(3));
        }
        mvprintw(y, col_x, "%9s", cached.formatted_size.c_str());
        if (!is_selected) {
            attroff(COLOR_PAIR(3));
        }
        col_x += 10;
        
        // Separator
        mvprintw(y, col_x, " | ");
        col_x += 3;
        
        // Percentage
        mvprintw(y, col_x, "%5.1f%%", cached.percentage);
        col_x += 8;
        
        // Separator and graph bar
        mvprintw(y, col_x, " | ");
        col_x += 3;
        
        // Graph bar
        int bar_width = static_cast<int>(cached.percentage / 100.0 * 20);
        bar_width = std::min(bar_width, 20);
        if (is_selected) {
            // Use a different character for selected bars
            for (int j = 0; j < bar_width; j++) {
                mvaddch(y, col_x + j, '=');
            }
        } else {
            attron(COLOR_PAIR(3));
            for (int j = 0; j < bar_width; j++) {
                mvaddch(y, col_x + j, ACS_CKBOARD);
            }
            attroff(COLOR_PAIR(3));
        }
        col_x += 20;
        
        // Optional columns
        if (show_mtime) {
            mvprintw(y, col_x, " | ");
            col_x += 3;
            mvprintw(y, col_x, "%19s", cached.formatted_time.c_str());
            col_x += 20;
        }
        
        if (show_count) {
            mvprintw(y, col_x, " | ");
            col_x += 3;
            
            if (entry->entry_count > 0) {
                mvprintw(y, col_x, "%7lu", entry->entry_count.load());
            } else {
                mvprintw(y, col_x, "      -");
            }
            col_x += 8;
        }
        
        // Name separator
        mvprintw(y, col_x, " | ");
        col_x += 3;
        
        // Name with appropriate styling for entry type
        if (entry->is_symlink && !is_selected) {
            attron(COLOR_PAIR(9));  // Magenta for symlinks
        } else if (entry->is_directory && !is_selected) {
            attron(COLOR_PAIR(1) | A_BOLD);
        }
        
        mvprintw(y, col_x, "%s", cached.formatted_name.c_str());
        
        if ((entry->is_symlink || entry->is_directory) && !is_selected) {
            attroff(COLOR_PAIR(entry->is_symlink ? 9 : 1) | (entry->is_directory ? A_BOLD : 0));
        }
        
        // Turn off selection highlighting
        if (is_selected) {
            attroff(COLOR_PAIR(4));
        }
        
        // Update cache
        if (index >= line_cache.size()) {
            line_cache.resize(index + 1);
        }
        line_cache[index].is_selected = is_selected;
    }
    
    void update_format_cache(std::shared_ptr<Entry> entry, CachedEntry& cached) {
        cached.formatted_size = format_size(entry->size, config.format);
        cached.percentage = (current_dir->size > 0) ? 
            (static_cast<double>(entry->size.load()) / current_dir->size.load() * 100.0) : 0.0;
        
        if (show_mtime && !entry->is_symlink) {  // Symlinks don't have meaningful mtime
            auto time_since_epoch = entry->last_modified.time_since_epoch();
            auto seconds = std::chrono::duration_cast<std::chrono::seconds>(time_since_epoch).count();
            time_t time_t_val = static_cast<time_t>(seconds);
            
            char time_buf[32];
            strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", 
                    localtime(&time_t_val));
            cached.formatted_time = time_buf;
        } else if (entry->is_symlink) {
            cached.formatted_time = "[symlink]";
        }
        
        std::string name = entry->path.filename().string();
        if (name.empty()) name = entry->path.string();
        
        if (entry->is_symlink) {
            // Format symlinks with an arrow pointing to their target
            // This is the key visual indicator that helps users understand what they're looking at
            cached.formatted_name = " " + name + " -> " + entry->symlink_target.string();
        } else if (entry->is_directory) {
            cached.formatted_name = "/" + name;
        } else {
            cached.formatted_name = " " + name;
        }
        
        // Adjust for available width
        int available_width = COLS - 45;
        if (show_mtime) available_width -= 23;
        if (show_count) available_width -= 11;
        
        if (cached.formatted_name.length() > static_cast<size_t>(available_width) && available_width > 3) {
            // For symlinks, try to preserve the arrow part if possible
            if (entry->is_symlink) {
                size_t arrow_pos = cached.formatted_name.find(" -> ");
                if (arrow_pos != std::string::npos) {
                    // Try to show both name and target with ellipsis
                    std::string symlink_part = cached.formatted_name.substr(0, arrow_pos);
                    std::string target_part = cached.formatted_name.substr(arrow_pos);
                    
                    int remaining = available_width - target_part.length() - 3;
                    if (remaining > 10) {
                        // Show end of symlink name and full target
                        cached.formatted_name = "..." + 
                            symlink_part.substr(symlink_part.length() - remaining + 3) + 
                            target_part;
                    } else {
                        // Just truncate the whole thing
                        cached.formatted_name = "..." + 
                            cached.formatted_name.substr(cached.formatted_name.length() - available_width + 3);
                    }
                }
            } else {
                cached.formatted_name = "..." + 
                    cached.formatted_name.substr(cached.formatted_name.length() - available_width + 3);
            }
        }
        
        cached.needs_update = false;
    }
    
    void update_status_line() {
        // Build current status
        std::string sort_str = "Sort mode: ";
        switch (sort_mode) {
            case SortMode::SIZE_DESC: sort_str += "size descending"; break;
            case SortMode::SIZE_ASC: sort_str += "size ascending"; break;
            case SortMode::NAME_ASC: sort_str += "name ascending"; break;
            case SortMode::NAME_DESC: sort_str += "name descending"; break;
            case SortMode::TIME_DESC: sort_str += "modified descending"; break;
            case SortMode::TIME_ASC: sort_str += "modified ascending"; break;
            case SortMode::COUNT_DESC: sort_str += "count descending"; break;
            case SortMode::COUNT_ASC: sort_str += "count ascending"; break;
        }
        
        size_t marked_count = count_marked_items();
        if (marked_count > 0) {
            uintmax_t marked_size = calculate_marked_size();
            sort_str += " | Marked: " + std::to_string(marked_count) + 
                       " items (" + format_size(marked_size, config.format) + ")";
        }
        
        // Only update if changed
        if (sort_str != last_status_line) {
            attron(A_REVERSE);
            move(LINES - 2, 0);
            clrtoeol();
            mvprintw(LINES - 2, 1, "%s", sort_str.c_str());
            attroff(A_REVERSE);
            last_status_line = sort_str;
        }
    }
    
    void draw_full() {
        clear();
        
        // Header
        attron(A_REVERSE);
        mvhline(0, 0, ' ', COLS);
        mvprintw(0, 1, " Disk Usage Analyzer v1.1.0 [C++ Optimized]    (press ? for help)");
        attroff(A_REVERSE);
        
        // Path bar
        attron(A_REVERSE);
        mvhline(1, 0, ' ', COLS);
        std::string path_str = current_dir->path.string();
        if (path_str.empty()) path_str = "[root]";
        mvprintw(1, 1, " %s", path_str.c_str());
        
        // Stats on the right
        if (!current_view.empty()) {
            std::string info = "(" + std::to_string(current_view.size()) + " visible, " +
                              std::to_string(current_dir->entry_count.load()) + " total, " +
                              format_size(current_dir->size, config.format) + ")";
            mvprintw(1, COLS - info.length() - 2, "%s", info.c_str());
        }
        attroff(A_REVERSE);
        
        // File list
        int y = 2;
        int max_y = LINES - 2;
        
        line_cache.clear();
        for (size_t i = view_offset; i < current_view.size() && y < max_y; i++) {
            draw_entry_line(i, y, true);
            y++;
        }
        
        // Status bar
        update_status_line();
        
        // Help line
        if (!glob_search_active && !show_help) {
            move(LINES - 1, 0);
            clrtoeol();
            mvprintw(LINES - 1, 1, " mark = d/space | delete = d | search = / | refresh = r/R");
        }
        
        if (show_help) {
            draw_help();
        }
        
        // Remember current state
        last_selected_index = selected_index;
        last_view_offset = view_offset;
        
        refresh();
    }
    
    void draw_help() {
        int help_y = LINES / 2 - 10;
        int help_x = COLS / 2 - 35;
        
        attron(COLOR_PAIR(7));
        for (int i = 0; i < 20; i++) {
            mvhline(help_y + i, help_x, ' ', 70);
        }
        
        attron(A_BOLD);
        mvprintw(help_y + 1, help_x + 30, "HELP");
        attroff(A_BOLD);
        
        int y = help_y + 3;
        mvprintw(y++, help_x + 2, "Navigation:");
        mvprintw(y++, help_x + 4, "↑/k         Move up");
        mvprintw(y++, help_x + 4, "↓/j         Move down");
        mvprintw(y++, help_x + 4, "→/l/Enter   Enter directory");
        mvprintw(y++, help_x + 4, "←/h/u       Go back");
        mvprintw(y++, help_x + 4, "O           Open with system");
        
        mvprintw(help_y + 3, help_x + 35, "Marking:");
        mvprintw(help_y + 4, help_x + 37, "space     Toggle mark");
        mvprintw(help_y + 5, help_x + 37, "d         Mark & move down");
        mvprintw(help_y + 6, help_x + 37, "a         Toggle all");
        mvprintw(help_y + 7, help_x + 37, "d         Delete marked");
        
        y++;
        mvprintw(y++, help_x + 2, "Sorting:");
        mvprintw(y++, help_x + 4, "s         By size");
        mvprintw(y++, help_x + 4, "n         By name");
        mvprintw(y++, help_x + 4, "m         By modified time");
        mvprintw(y++, help_x + 4, "c         By entry count");
        
        mvprintw(help_y + 10, help_x + 35, "Display:");
        mvprintw(help_y + 11, help_x + 37, "M         Toggle mtime");
        mvprintw(help_y + 12, help_x + 37, "C         Toggle count");
        mvprintw(help_y + 13, help_x + 37, "/         Glob search");
        mvprintw(help_y + 14, help_x + 37, "r/R       Refresh");
        
        mvprintw(help_y + 17, help_x + 20, "Press any key to close help");
        
        attroff(COLOR_PAIR(7));
    }
    
    // Handle non-movement keys
    bool handle_key(int ch) {
        switch (ch) {
            case KEY_RIGHT:
            case KEY_ENTER:
            case '\n':
            case 'l':
            case 'o':
                enter_directory();
                break;
                
            case KEY_LEFT:
            case 'h':
            case KEY_BACKSPACE:
            case 'u':
                exit_directory();
                break;
                
            case ' ':
                toggle_mark();
                break;
                
            case 'a':
            case 'A':
                toggle_all_marks();
                needs_full_redraw = true;
                break;
                
            case 'd':
                if (has_marked_items()) {
                    delete_marked_entries();
                    needs_full_redraw = true;
                } else if (selected_index < current_view.size()) {
                    current_view[selected_index]->marked = true;
                    navigate_down();
                }
                break;
                
            case 'O':  // Open with system
                open_selected();
                break;
                
            case '/':  // Glob search
                start_glob_search();
                needs_full_redraw = true;
                break;
                
            case 'r':  // Refresh selected
                refresh_selected();
                needs_full_redraw = true;
                break;
                
            case 'R':  // Refresh all
                refresh_all();
                needs_full_redraw = true;
                break;
                
            case '?':
                show_help = !show_help;
                needs_full_redraw = true;
                break;
                
            case 'q':
            case 'Q':
                return false;  // Exit
                
            case 's':
                sort_by_size();
                needs_full_redraw = true;
                break;
                
            case 'n':
                sort_by_name();
                needs_full_redraw = true;
                break;
                
            case 'm':
                sort_by_time();
                needs_full_redraw = true;
                break;
                
            case 'c':
                sort_by_count();
                needs_full_redraw = true;
                break;
                
            case 'M':
                show_mtime = !show_mtime;
                format_cache.clear();
                needs_full_redraw = true;
                break;
                
            case 'C':
                show_count = !show_count;
                format_cache.clear();
                needs_full_redraw = true;
                break;
                
            case 'g':
            case 'S':
                // Cycle visualization mode (would need to implement)
                break;
                
            default:
                break;
        }
        
        return true;  // Continue running
    }
    
    void sort_by_size() {
        sort_mode = (sort_mode == SortMode::SIZE_DESC) ? 
                    SortMode::SIZE_ASC : SortMode::SIZE_DESC;
        apply_sort();
    }
    
    void sort_by_name() {
        sort_mode = (sort_mode == SortMode::NAME_ASC) ? 
                    SortMode::NAME_DESC : SortMode::NAME_ASC;
        apply_sort();
    }
    
    void sort_by_time() {
        sort_mode = (sort_mode == SortMode::TIME_DESC) ? 
                    SortMode::TIME_ASC : SortMode::TIME_DESC;
        apply_sort();
    }
    
    void sort_by_count() {
        sort_mode = (sort_mode == SortMode::COUNT_DESC) ? 
                    SortMode::COUNT_ASC : SortMode::COUNT_DESC;
        apply_sort();
    }
    
    void toggle_mark() {
        if (selected_index < current_view.size()) {
            auto entry = current_view[selected_index];
            entry->marked = !entry->marked.load();
        }
    }
    
    void toggle_all_marks() {
        bool any_marked = has_marked_items();
        for (auto& entry : current_view) {
            entry->marked = !any_marked;
        }
    }
    
    bool has_marked_items() {
        for (const auto& entry : current_view) {
            if (entry->marked.load()) {
                return true;
            }
        }
        return false;
    }
    
    size_t count_marked_items() {
        size_t count = 0;
        count_marked_recursive(current_dir, count);
        return count;
    }
    
    void count_marked_recursive(std::shared_ptr<Entry> root, size_t& count) {
        if (root->marked.load()) {
            count++;
        }
        
        if (root->is_directory && !root->is_symlink) {
            std::lock_guard<std::mutex> lock(root->children_mutex);
            for (auto& child : root->children) {
                count_marked_recursive(child, count);
            }
        }
    }
    
    uintmax_t calculate_marked_size() {
        uintmax_t total = 0;
        calculate_marked_size_recursive(current_dir, total);
        return total;
    }
    
    void calculate_marked_size_recursive(std::shared_ptr<Entry> root, uintmax_t& total) {
        if (root->marked.load()) {
            total += root->size.load();
        } else if (root->is_directory && !root->is_symlink) {
            std::lock_guard<std::mutex> lock(root->children_mutex);
            for (auto& child : root->children) {
                calculate_marked_size_recursive(child, total);
            }
        }
    }
    
    void delete_marked_entries() {
        std::vector<std::shared_ptr<Entry>> marked_entries;
        collect_marked_entries(current_dir, marked_entries);
        
        if (marked_entries.empty()) return;
        
        // Confirmation
        clear();
        mvprintw(LINES / 2 - 3, COLS / 2 - 20, "Delete %zu marked items?", 
                marked_entries.size());
        mvprintw(LINES / 2 - 1, COLS / 2 - 20, "Total size: %s", 
                format_size(calculate_marked_size(), config.format).c_str());
        mvprintw(LINES / 2 + 1, COLS / 2 - 20, "This action cannot be undone!");
        mvprintw(LINES / 2 + 3, COLS / 2 - 20, "Press 'y' to confirm, any other key to cancel");
        refresh();
        
        int ch = getch();
        if (ch == 'y' || ch == 'Y') {
            size_t deleted_count = 0;
            
            for (auto& entry : marked_entries) {
                try {
                    if (entry->is_directory && !entry->is_symlink) {
                        fs::remove_all(entry->path);
                    } else {
                        fs::remove(entry->path);
                    }
                    deleted_count++;
                    
                    // Remove from parent
                    remove_from_parent(entry);
                } catch (...) {
                    // Continue with other files
                }
            }
            
            refresh_all();
            
            clear();
            mvprintw(LINES / 2, COLS / 2 - 20, "Deleted %zu items successfully!", 
                    deleted_count);
            refresh();
            getch();
        }
    }
    
    void collect_marked_entries(std::shared_ptr<Entry> root, 
                               std::vector<std::shared_ptr<Entry>>& marked) {
        if (root->marked.load()) {
            marked.push_back(root);
        } else if (root->is_directory && !root->is_symlink) {
            std::lock_guard<std::mutex> lock(root->children_mutex);
            for (auto& child : root->children) {
                collect_marked_entries(child, marked);
            }
        }
    }
    
    void remove_from_parent(std::shared_ptr<Entry> entry) {
        // This is simplified - in a real implementation we'd need to track parent pointers
        // For now, just mark it as deleted
        entry->size = 0;
        entry->entry_count = 0;
    }
    
    void print_marked_paths() {
        std::vector<std::shared_ptr<Entry>> marked_entries;
        
        for (auto& root : roots) {
            collect_marked_entries(root, marked_entries);
        }
        
        for (auto& entry : marked_entries) {
            std::cout << entry->path << "\n";
        }
    }
};

// Tree printing function with symlink support
void print_tree_sorted(const std::shared_ptr<Entry>& entry, const Config& config,
                      const std::string& prefix, bool is_last, 
                      int depth, std::vector<std::shared_ptr<Entry>>* siblings) {
    // Check depth limit
    if (config.max_depth >= 0 && depth > config.max_depth) return;
    
    // Print current entry with tree graphics
    std::cout << prefix;
    
    // Tree branch characters
    if (depth > 0) {
        std::cout << (is_last ? "└── " : "├── ");
    }
    
    // Apply appropriate coloring
    if (!config.no_colors) {
        if (entry->is_symlink) {
            std::cout << MAGENTA;  // Magenta for symlinks
        } else if (entry->is_directory) {
            std::cout << BLUE << BOLD;
        }
    }
    
    // Print the name
    std::string name = entry->path.filename().string();
    if (name.empty() && depth == 0) {
        name = entry->path.string();
    }
    
    std::cout << name;
    
    // For symlinks, show the target - this makes it clear what the symlink points to
    if (entry->is_symlink) {
        std::cout << " -> " << entry->symlink_target.string();
    }
    
    if (!config.no_colors && (entry->is_symlink || entry->is_directory)) {
        std::cout << RESET;
    }
    
    // Print size information (symlinks show as 0)
    std::cout << " ";
    
    if (!config.no_colors) {
        std::cout << YELLOW;
    }
    
    std::cout << "[" << format_size(entry->size, config.format) << "]";
    
    if (!config.no_colors) {
        std::cout << RESET;
    }
    
    std::cout << "\n";
    
    // Only process children for directories (not symlinks, even if they point to directories)
    if (entry->is_directory && !entry->is_symlink) {
        // Get and sort children
        std::vector<std::shared_ptr<Entry>> children_copy;
        {
            std::lock_guard<std::mutex> lock(entry->children_mutex);
            children_copy = entry->children;
        }
        
        // Sort children by size (descending)
        std::sort(children_copy.begin(), children_copy.end(),
            [](const std::shared_ptr<Entry>& a, const std::shared_ptr<Entry>& b) {
                return a->size.load() > b->size.load();
            });
        
        // Apply top-n limit if specified
        size_t limit = children_copy.size();
        if (config.top_n > 0 && limit > static_cast<size_t>(config.top_n)) {
            limit = static_cast<size_t>(config.top_n);
        }
        
        // Print children recursively
        for (size_t i = 0; i < limit; i++) {
            bool child_is_last = (i == limit - 1);
            std::string child_prefix = prefix + (is_last ? "    " : "│   ");
            
            print_tree_sorted(children_copy[i], config, child_prefix, child_is_last, 
                             depth + 1, &children_copy);
        }
        
        // Show omitted entries if any
        if (config.top_n > 0 && children_copy.size() > static_cast<size_t>(config.top_n)) {
            std::string omit_prefix = prefix + (is_last ? "    " : "│   ");
            std::cout << omit_prefix << "└── ";
            if (!config.no_colors) {
                std::cout << GRAY;
            }
            std::cout << "... " << (children_copy.size() - limit) << " more entries";
            if (!config.no_colors) {
                std::cout << RESET;
            }
            std::cout << "\n";
        }
    }
}

// Aggregate mode (non-interactive summary)
void aggregate_mode(Config& config) {
    WorkStealingThreadPool pool(config.thread_count);
    OptimizedScanner scanner(pool, config);
    
    auto roots = scanner.scan(config.paths);
    
    // Check if tree mode is requested
    if (config.tree_mode) {
        // Tree mode - show hierarchical view
        std::cout << "\n";  // Add spacing before tree output
        
        if (roots.size() == 1) {
            // Single root - print it directly
            print_tree_sorted(roots[0], config);
        } else {
            // Multiple roots - create a virtual root
            auto virtual_root = std::make_shared<Entry>("[Total]");
            virtual_root->is_directory = true;
            
            for (auto& root : roots) {
                virtual_root->children.push_back(root);
                virtual_root->size += root->size.load();
                virtual_root->entry_count += root->entry_count.load();
            }
            
            // Sort the roots by size
            std::sort(virtual_root->children.begin(), virtual_root->children.end(),
                [](const std::shared_ptr<Entry>& a, const std::shared_ptr<Entry>& b) {
                    return a->size.load() > b->size.load();
                });
            
            print_tree_sorted(virtual_root, config);
        }
        
        std::cout << "\n";  // Add spacing after tree output
    } else {
        // Flat mode - original aggregate output
        // Sort by size if requested
        std::sort(roots.begin(), roots.end(),
            [](const std::shared_ptr<Entry>& a, const std::shared_ptr<Entry>& b) {
                return a->size.load() < b->size.load();
            });
        
        // Print results
        for (auto& root : roots) {
            std::cout << std::setw(12) << std::right 
                      << format_size(root->size, config.format) << " ";
            
            if (!config.no_colors) {
                if (root->is_symlink) {
                    std::cout << MAGENTA;
                } else if (root->is_directory) {
                    std::cout << CYAN;
                }
            }
            
            std::cout << root->path;
            
            // Show symlink target in aggregate mode too
            if (root->is_symlink) {
                std::cout << " -> " << root->symlink_target.string();
            }
            
            if (!config.no_colors && (root->is_symlink || root->is_directory)) {
                std::cout << RESET;
            }
            
            std::cout << "\n";
        }
        
        // Print total if multiple paths
        if (roots.size() > 1) {
            uintmax_t total = 0;
            for (auto& root : roots) {
                total += root->size.load();
            }
            
            std::cout << std::setw(12) << std::right 
                      << format_size(total, config.format) << " ";
            std::cout << "total\n";
        }
    }
    
    scanner.print_stats();
}

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [SUBCOMMAND] [OPTIONS] [PATH...]\n\n";
    std::cout << "A tool to conveniently learn about disk usage, fast!\n\n";
    std::cout << "Subcommands:\n";
    std::cout << "  i, interactive    Launch interactive mode\n";
    std::cout << "  a, aggregate      Aggregate disk usage (default)\n\n";
    std::cout << "Options:\n";
    std::cout << "  -h, --help              Show this help message\n";
    std::cout << "  -A, --apparent-size     Display apparent size instead of disk usage\n";
    std::cout << "  -l, --count-hard-links  Count hard-linked files each time they are seen\n";
    std::cout << "  -x, --stay-on-filesystem Don't cross filesystem boundaries\n";
    std::cout << "  -d, --depth N           Maximum depth to traverse\n";
    std::cout << "  -t, --top N             Show only top N entries by size\n";
    std::cout << "  -T, --tree              Display results as a tree (aggregate mode)\n";
    std::cout << "  -f, --format FMT        Output format: metric, binary, bytes, gb, gib, mb, mib\n";
    std::cout << "  -j, --threads N         Number of threads (default: auto)\n";
    std::cout << "  -i, --ignore-dirs DIR   Directories to ignore (can be repeated)\n";
    std::cout << "  --no-entry-check        Don't check entries for presence (faster but may show stale data)\n";
    std::cout << "  --no-colors             Disable colored output\n";
    std::cout << "  --no-progress           Disable progress reporting\n\n";
    std::cout << "If no path is provided, the current directory is used.\n";
}

int main(int argc, char* argv[]) {
    Config config;
    std::string subcommand;
    
    // Parse arguments
    std::vector<std::string> args(argv + 1, argv + argc);
    
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& arg = args[i];
        
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "i" || arg == "interactive") {
            config.interactive_mode = true;
            subcommand = "interactive";
        } else if (arg == "a" || arg == "aggregate") {
            subcommand = "aggregate";
        } else if (arg == "-A" || arg == "--apparent-size") {
            config.apparent_size = true;
        } else if (arg == "-l" || arg == "--count-hard-links") {
            config.count_hard_links = true;
        } else if (arg == "-x" || arg == "--stay-on-filesystem") {
            config.stay_on_filesystem = true;
        } else if (arg == "--no-entry-check") {
            config.no_entry_check = true;
        } else if (arg == "--no-colors") {
            config.no_colors = true;
        } else if (arg == "--no-progress") {
            config.show_progress = false;
        } else if (arg == "-d" || arg == "--depth") {
            if (i + 1 < args.size()) {
                config.max_depth = std::stoi(args[++i]);
            }
        } else if (arg == "-t" || arg == "--top") {
            if (i + 1 < args.size()) {
                config.top_n = std::stoi(args[++i]);
            }
        } else if (arg == "-T" || arg == "--tree") {
            config.tree_mode = true;
        } else if (arg == "-f" || arg == "--format") {
            if (i + 1 < args.size()) {
                config.format = args[++i];
                std::transform(config.format.begin(), config.format.end(), 
                             config.format.begin(), ::tolower);
            }
        } else if (arg == "-j" || arg == "--threads") {
            if (i + 1 < args.size()) {
                config.thread_count = std::stoi(args[++i]);
            }
        } else if (arg == "-i" || arg == "--ignore-dirs") {
            if (i + 1 < args.size()) {
                try {
                    config.ignore_dirs.insert(fs::canonical(args[++i]));
                } catch (...) {
                    std::cerr << "Warning: Cannot resolve ignore directory: " << args[i] << "\n";
                }
            }
        } else if (arg[0] != '-') {
            config.paths.push_back(arg);
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            std::cerr << "Try '" << argv[0] << " --help' for more information.\n";
            return 1;
        }
    }
    
    // Default to current directory if no paths specified
    if (config.paths.empty()) {
        config.paths.push_back(".");
    }
    
    // Validate paths
    for (const auto& path : config.paths) {
        if (!fs::exists(path)) {
            std::cerr << "Error: Path does not exist: " << path << "\n";
            return 1;
        }
    }
    
    // Default to interactive mode if no subcommand specified and on TTY
    // BUT: if tree mode is requested, always use aggregate mode
    if (subcommand.empty() && !config.tree_mode && isatty(fileno(stdout))) {
        config.interactive_mode = true;
    }
    
    // Run appropriate mode
    if (config.interactive_mode) {
        WorkStealingThreadPool pool(config.thread_count);
        OptimizedScanner scanner(pool, config);
        
        auto start = std::chrono::high_resolution_clock::now();
        auto roots = scanner.scan(config.paths);
        auto end = std::chrono::high_resolution_clock::now();
        
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        
        InteractiveUI ui(roots, config);
        ui.run();
    } else {
        aggregate_mode(config);
    }
    
    return 0;
}
