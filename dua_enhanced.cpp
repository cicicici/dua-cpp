// dua_enhanced.cpp - Enhanced Disk Usage Analyzer in C++
// Reimplementation of dua-cli with additional features

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
constexpr size_t QUEUE_SIZE_LIMIT = 10000;
constexpr size_t PREALLOCATE_ENTRIES = 100;

// ANSI color codes
const std::string RESET = "\033[0m";
const std::string RED = "\033[31m";
const std::string GREEN = "\033[32m";
const std::string YELLOW = "\033[33m";
const std::string BLUE = "\033[34m";
const std::string CYAN = "\033[36m";
const std::string BOLD = "\033[1m";
const std::string GRAY = "\033[90m";

// Configuration structure
struct Config {
    bool interactive_mode = false;
    bool apparent_size = false;
    bool count_hard_links = false;
    bool stay_on_filesystem = false;
    bool no_entry_check = false;
    bool use_trash = false;
    bool no_colors = true;  // ncurses handles colors in interactive mode
    int max_depth = -1;
    int top_n = -1;
    size_t thread_count = 0;
    std::string format = "metric";  // metric, binary, bytes, gb, gib, mb, mib
    std::set<fs::path> ignore_dirs;
    std::vector<fs::path> paths;
};

// Enhanced entry structure with more metadata
struct Entry {
    fs::path path;
    std::atomic<uintmax_t> size{0};
    std::atomic<uintmax_t> apparent_size{0};
    bool is_directory{false};
    bool is_symlink{false};
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
                if (!is_symlink) {
                    last_modified = fs::last_write_time(path);
                    
                    // Get inode information
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

// High-performance thread pool (reused from original)
class ThreadPool {
private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    std::condition_variable finished;
    std::atomic<bool> stop{false};
    std::atomic<size_t> active_tasks{0};
    std::atomic<size_t> queued_tasks{0};
    
public:
    ThreadPool(size_t threads = 0) {
        if (threads == 0) {
            threads = std::thread::hardware_concurrency();
            if (threads == 0) threads = 4;
            
            // Special handling for macOS - use 3 threads for better performance
#ifdef __APPLE__
            threads = 3;
#endif
        }
        
        for (size_t i = 0; i < threads; ++i) {
            workers.emplace_back([this] {
                for (;;) {
                    std::function<void()> task;
                    
                    {
                        std::unique_lock<std::mutex> lock(queue_mutex);
                        condition.wait(lock, [this] { return stop || !tasks.empty(); });
                        
                        if (stop && tasks.empty()) return;
                        
                        task = std::move(tasks.front());
                        tasks.pop();
                        queued_tasks--;
                        active_tasks++;
                    }
                    
                    task();
                    active_tasks--;
                    finished.notify_all();
                }
            });
        }
    }
    
    ~ThreadPool() {
        stop = true;
        condition.notify_all();
        for (auto& worker : workers) {
            worker.join();
        }
    }
    
    template<class F>
    void enqueue(F&& f) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            finished.wait(lock, [this] { return queued_tasks < QUEUE_SIZE_LIMIT || stop; });
            if (stop) return;
            
            tasks.emplace(std::forward<F>(f));
            queued_tasks++;
        }
        condition.notify_one();
    }
    
    void wait_all() {
        std::unique_lock<std::mutex> lock(queue_mutex);
        finished.wait(lock, [this] { return tasks.empty() && active_tasks == 0; });
    }
};

// Enhanced scanner with hard link tracking
class OptimizedScanner {
private:
    ThreadPool& pool;
    Config& config;
    std::atomic<uintmax_t> total_size{0};
    std::atomic<size_t> file_count{0};
    std::atomic<size_t> dir_count{0};
    std::atomic<size_t> io_errors{0};
    std::chrono::steady_clock::time_point start_time;
    
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
        
        return config.ignore_dirs.find(canonical_path) != config.ignore_dirs.end();
    }
    
    void scan_directory_impl(std::shared_ptr<Entry> entry, dev_t root_device) {
        try {
            if (entry->is_symlink) {
                return;
            }
            
            if (should_ignore_directory(entry->path)) {
                return;
            }
            
            std::vector<fs::directory_entry> entries;
            entries.reserve(BATCH_SIZE);
            
            for (const auto& item : fs::directory_iterator(entry->path, 
                    fs::directory_options::skip_permission_denied)) {
                entries.push_back(item);
            }
            
            for (const auto& item : entries) {
                try {
                    auto child = std::make_shared<Entry>(item.path());
                    
                    // Check filesystem boundary
                    if (config.stay_on_filesystem && child->device_id != root_device) {
                        continue;
                    }
                    
                    if (child->is_symlink) {
                        continue;
                    }
                    
                    if (item.is_directory()) {
                        child->is_directory = true;
                        dir_count++;
                        
                        {
                            std::lock_guard<std::mutex> lock(entry->children_mutex);
                            entry->children.push_back(child);
                        }
                        
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
                            entry->entry_count++;
                        }
                        
                        {
                            std::lock_guard<std::mutex> lock(entry->children_mutex);
                            entry->children.push_back(child);
                        }
                        
                        entry->size += child->size.load();
                    }
                } catch (const fs::filesystem_error&) {
                    io_errors++;
                }
            }
        } catch (const fs::filesystem_error&) {
            io_errors++;
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
    OptimizedScanner(ThreadPool& tp, Config& cfg) : pool(tp), config(cfg) {
        start_time = std::chrono::steady_clock::now();
    }
    
    std::vector<std::shared_ptr<Entry>> scan(const std::vector<fs::path>& paths) {
        std::vector<std::shared_ptr<Entry>> roots;
        
        for (const auto& path : paths) {
            auto root = std::make_shared<Entry>(path);
            root->is_directory = fs::is_directory(path);
            
            if (root->is_directory) {
                dir_count++;
                scan_directory_impl(root, root->device_id);
            } else {
                root->apparent_size = fs::file_size(path);
                root->size = config.apparent_size ? root->apparent_size.load() : 
                           get_size_on_disk(path, root->apparent_size);
                file_count++;
            }
            
            roots.push_back(root);
        }
        
        pool.wait_all();
        
        for (auto& root : roots) {
            total_size += calculate_sizes(root);
        }
        
        return roots;
    }
    
    void print_stats() {
        auto duration = std::chrono::steady_clock::now() - start_time;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
        
        std::cout << "\nScanned " << file_count << " files and " 
                  << dir_count << " directories in " << ms << "ms\n";
        if (io_errors > 0) {
            std::cout << "Encountered " << io_errors << " I/O errors\n";
        }
        std::cout << "Total size: " << format_size(total_size, config.format) << "\n";
    }
};

// Interactive UI with enhanced features
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
    }
    
    void run() {
        initscr();
        cbreak();
        noecho();
        keypad(stdscr, TRUE);
        curs_set(0);
        
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
        }
        
        bool running = true;
        while (running) {
            draw();
            
            if (glob_search_active) {
                handle_glob_search();
            } else {
                int ch = getch();
                
                switch (ch) {
                    case KEY_UP:
                    case 'k':
                        navigate_up();
                        break;
                        
                    case KEY_DOWN:
                    case 'j':
                        navigate_down();
                        break;
                        
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
                        break;
                        
                    case 'd':
                        if (has_marked_items()) {
                            delete_marked_entries();
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
                        break;
                        
                    case 'r':  // Refresh selected
                        refresh_selected();
                        break;
                        
                    case 'R':  // Refresh all
                        refresh_all();
                        break;
                        
                    case '?':
                        show_help = !show_help;
                        break;
                        
                    case 'q':
                    case 'Q':
                        running = false;
                        break;
                        
                    case 's':
                        sort_by_size();
                        break;
                        
                    case 'n':
                        sort_by_name();
                        break;
                        
                    case 'm':
                        sort_by_time();
                        break;
                        
                    case 'c':
                        sort_by_count();
                        break;
                        
                    case 'M':
                        show_mtime = !show_mtime;
                        break;
                        
                    case 'C':
                        show_count = !show_count;
                        break;
                        
                    case 'g':
                    case 'S':
                        // Cycle visualization mode (would need to implement)
                        break;
                }
            }
        }
        
        endwin();
        
        // Print marked paths on exit
        print_marked_paths();
    }
    
private:
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
            if (selected->is_directory && !selected->children.empty()) {
                current_dir = selected;
                navigation_stack.push_back(current_dir);
                update_view();
                selected_index = 0;
                view_offset = 0;
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
    
    void handle_glob_search() {
        // Show search prompt
        mvprintw(LINES - 1, 0, "Search: %s", glob_pattern.c_str());
        refresh();
        
        int ch = getch();
        if (ch == 27) {  // ESC
            glob_search_active = false;
        } else if (ch == '\n') {
            perform_glob_search();
            glob_search_active = false;
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
        
        if (root->is_directory) {
            std::lock_guard<std::mutex> lock(root->children_mutex);
            for (auto& child : root->children) {
                search_entries(child, pattern, matches);
            }
        }
    }
    
    void refresh_selected() {
        if (selected_index < current_view.size()) {
            auto selected = current_view[selected_index];
            if (selected->is_directory) {
                // Re-scan directory
                ThreadPool pool(config.thread_count);
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
        ThreadPool pool(config.thread_count);
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
    
    void draw() {
        clear();
        
        // Header
        attron(A_REVERSE);
        mvhline(0, 0, ' ', COLS);
        mvprintw(0, 1, " Disk Usage Analyzer v2.30.1 [C++]    (press ? for help)");
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
        
        // Calculate column positions
        int col_x = 0;
        int size_col_width = 10;
        int percent_col_width = 8;
        int graph_col_width = 20;
        int mtime_col_width = show_mtime ? 20 : 0;
        int count_col_width = show_count ? 8 : 0;
        
        for (size_t i = view_offset; i < current_view.size() && y < max_y; i++) {
            auto entry = current_view[i];
            
            // Highlight selected
            if (i == selected_index) {
                attron(COLOR_PAIR(4));
                mvhline(y, 0, ' ', COLS);
            }
            
            col_x = 1;
            
            // Mark indicator
            if (entry->marked.load()) {
                attron(COLOR_PAIR(8) | A_BOLD);
                mvprintw(y, 0, "*");
                attroff(COLOR_PAIR(8) | A_BOLD);
            }
            
            // Size
            attron(COLOR_PAIR(3));
            mvprintw(y, col_x, "%9s", format_size(entry->size, config.format).c_str());
            attroff(COLOR_PAIR(3));
            col_x += size_col_width;
            
            mvprintw(y, col_x, " | ");
            col_x += 3;
            
            // Percentage
            double percentage = (current_dir->size > 0) ? 
                (static_cast<double>(entry->size.load()) / current_dir->size.load() * 100.0) : 0.0;
            mvprintw(y, col_x, "%5.1f%%", percentage);
            col_x += percent_col_width;
            
            mvprintw(y, col_x, " | ");
            col_x += 3;
            
            // Graph bar
            int bar_width = static_cast<int>(percentage / 100.0 * graph_col_width);
            bar_width = std::min(bar_width, graph_col_width);
            attron(COLOR_PAIR(3));
            for (int j = 0; j < bar_width; j++) {
                mvaddch(y, col_x + j, ACS_CKBOARD);
            }
            attroff(COLOR_PAIR(3));
            col_x += graph_col_width;
            
            // Modified time
            if (show_mtime) {
                mvprintw(y, col_x, " | ");
                col_x += 3;
                
                // C++17 compatible time conversion
                auto time_since_epoch = entry->last_modified.time_since_epoch();
                auto seconds = std::chrono::duration_cast<std::chrono::seconds>(time_since_epoch).count();
                time_t time_t_val = static_cast<time_t>(seconds);
                
                char time_buf[32];
                strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", 
                        localtime(&time_t_val));
                mvprintw(y, col_x, "%19s", time_buf);
                col_x += mtime_col_width;
            }
            
            // Entry count
            if (show_count) {
                mvprintw(y, col_x, " | ");
                col_x += 3;
                
                if (entry->entry_count > 0) {
                    mvprintw(y, col_x, "%7lu", entry->entry_count.load());
                } else {
                    mvprintw(y, col_x, "      -");
                }
                col_x += count_col_width;
            }
            
            mvprintw(y, col_x, " | ");
            col_x += 3;
            
            // Name
            if (entry->is_directory) {
                attron(COLOR_PAIR(1) | A_BOLD);
                mvprintw(y, col_x, "/");
            } else {
                mvprintw(y, col_x, " ");
            }
            
            std::string name = entry->path.filename().string();
            if (name.empty()) name = entry->path.string();
            int max_name_width = COLS - col_x - 2;
            if (name.length() > static_cast<size_t>(max_name_width)) {
                name = "..." + name.substr(name.length() - max_name_width + 3);
            }
            mvprintw(y, col_x + 1, "%s", name.c_str());
            
            if (entry->is_directory) {
                attroff(COLOR_PAIR(1) | A_BOLD);
            }
            
            if (i == selected_index) {
                attroff(COLOR_PAIR(4));
            }
            
            y++;
        }
        
        // Status bar
        attron(A_REVERSE);
        mvhline(LINES - 2, 0, ' ', COLS);
        
        // Sort mode
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
        mvprintw(LINES - 2, 1, "%s", sort_str.c_str());
        
        // Marked items info
        size_t marked_count = count_marked_items();
        if (marked_count > 0) {
            uintmax_t marked_size = calculate_marked_size();
            std::string marked_str = " | Marked: " + std::to_string(marked_count) + 
                                   " items (" + format_size(marked_size, config.format) + ")";
            mvprintw(LINES - 2, 25, "%s", marked_str.c_str());
        }
        
        attroff(A_REVERSE);
        
        if (show_help) {
            draw_help();
        } else if (!glob_search_active) {
            // Help hints
            mvprintw(LINES - 1, 1, " mark = d/space | delete = d | search = / | refresh = r/R");
        }
        
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
        
        if (root->is_directory) {
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
        } else if (root->is_directory) {
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
                    if (entry->is_directory) {
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
        } else if (root->is_directory) {
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

// Aggregate mode (non-interactive summary)
void aggregate_mode(Config& config) {
    ThreadPool pool(config.thread_count);
    OptimizedScanner scanner(pool, config);
    
    auto roots = scanner.scan(config.paths);
    
    // Sort by size if requested
    std::sort(roots.begin(), roots.end(),
        [](const std::shared_ptr<Entry>& a, const std::shared_ptr<Entry>& b) {
            return a->size.load() < b->size.load();
        });
    
    // Print results
    for (auto& root : roots) {
        std::cout << std::setw(12) << std::right 
                  << format_size(root->size, config.format) << " ";
        
        if (!config.no_colors && root->is_directory) {
            std::cout << CYAN;
        }
        
        std::cout << root->path;
        
        if (!config.no_colors && root->is_directory) {
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
    std::cout << "  -f, --format FMT        Output format: metric, binary, bytes, gb, gib, mb, mib\n";
    std::cout << "  -j, --threads N         Number of threads (default: auto)\n";
    std::cout << "  -i, --ignore-dirs DIR   Directories to ignore (can be repeated)\n";
    std::cout << "  --no-entry-check        Don't check entries for presence (faster but may show stale data)\n";
    std::cout << "  --no-colors             Disable colored output\n\n";
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
        } else if (arg == "-d" || arg == "--depth") {
            if (i + 1 < args.size()) {
                config.max_depth = std::stoi(args[++i]);
            }
        } else if (arg == "-t" || arg == "--top") {
            if (i + 1 < args.size()) {
                config.top_n = std::stoi(args[++i]);
            }
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
    
    // Default to interactive mode if no subcommand specified
    if (subcommand.empty() && isatty(fileno(stdout))) {
        config.interactive_mode = true;
    }
    
    // Run appropriate mode
    if (config.interactive_mode) {
        std::cout << "Scanning...\n";
        
        ThreadPool pool(config.thread_count);
        OptimizedScanner scanner(pool, config);
        
        auto start = std::chrono::high_resolution_clock::now();
        auto roots = scanner.scan(config.paths);
        auto end = std::chrono::high_resolution_clock::now();
        
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        std::cout << "\nScan completed in " << duration.count() << "ms\n";
        scanner.print_stats();
        
        InteractiveUI ui(roots, config);
        ui.run();
    } else {
        aggregate_mode(config);
    }
    
    return 0;
}
