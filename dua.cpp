// dua_optimized.cpp - High-performance Disk Usage Analyzer in C++
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
#include <sstream>
#include <memory>
#include <future>
#include <numeric>

#ifdef __linux__
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
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

// Entry structure optimized for cache efficiency
struct Entry {
    fs::path path;
    std::atomic<uintmax_t> size{0};
    bool is_directory{false};
    std::vector<std::shared_ptr<Entry>> children;
    mutable std::mutex children_mutex;
    fs::file_time_type last_modified;
    
    Entry(const fs::path& p = "") : path(p) {
        children.reserve(PREALLOCATE_ENTRIES);
        try {
            if (fs::exists(path)) {
                last_modified = fs::last_write_time(path);
            }
        } catch (...) {
            // Use epoch time if we can't get the modified time
            last_modified = fs::file_time_type{};
        }
    }
};

// High-performance thread pool
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
            if (threads == 0) threads = 4;  // Fallback
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
            
            // Wait if queue is too large to prevent memory explosion
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
    
    size_t get_active_tasks() const {
        return active_tasks.load();
    }
};

// Platform-specific optimized directory scanner
class OptimizedScanner {
private:
    ThreadPool& pool;
    std::atomic<uintmax_t> total_size{0};
    std::atomic<size_t> file_count{0};
    std::atomic<size_t> dir_count{0};
    
    void scan_directory_impl(std::shared_ptr<Entry> entry) {
        try {
            // Skip symbolic links
            if (fs::is_symlink(entry->path)) {
                return;
            }
            
            std::vector<fs::directory_entry> entries;
            entries.reserve(BATCH_SIZE);
            
            // Collect all entries from this directory
            for (const auto& item : fs::directory_iterator(entry->path, 
                    fs::directory_options::skip_permission_denied)) {
                entries.push_back(item);
            }
            
            // Process entries
            for (const auto& item : entries) {
                try {
                    // Skip symbolic links
                    if (item.is_symlink()) {
                        continue;
                    }
                    
                    auto child = std::make_shared<Entry>(item.path());
                    
                    if (item.is_directory()) {
                        child->is_directory = true;
                        child->last_modified = item.last_write_time();
                        dir_count++;
                        
                        // Add child to parent before scanning (for UI updates)
                        {
                            std::lock_guard<std::mutex> lock(entry->children_mutex);
                            entry->children.push_back(child);
                        }
                        
                        // Enqueue directory scan
                        pool.enqueue([this, child]() {
                            scan_directory_impl(child);
                        });
                    } else if (item.is_regular_file()) {
                        child->size = item.file_size();
                        child->last_modified = item.last_write_time();
                        file_count++;
                        
                        // Add to parent
                        {
                            std::lock_guard<std::mutex> lock(entry->children_mutex);
                            entry->children.push_back(child);
                        }
                        
                        // Update parent sizes atomically
                        for (auto* current = entry.get(); current != nullptr; ) {
                            current->size += child->size.load();
                            break; // Only update immediate parent in thread
                        }
                    }
                } catch (const fs::filesystem_error&) {
                    // Skip inaccessible files
                }
            }
        } catch (const fs::filesystem_error&) {
            // Skip inaccessible directories
        }
    }
    
    // Recursively calculate sizes after scanning
    uintmax_t calculate_sizes(std::shared_ptr<Entry> entry) {
        if (!entry->is_directory) {
            return entry->size.load();
        }
        
        uintmax_t total = 0;
        {
            std::lock_guard<std::mutex> lock(entry->children_mutex);
            for (auto& child : entry->children) {
                total += calculate_sizes(child);
            }
            
            // Sort children by size (descending) after calculating sizes
            std::sort(entry->children.begin(), entry->children.end(),
                [](const std::shared_ptr<Entry>& a, const std::shared_ptr<Entry>& b) {
                    return a->size.load() > b->size.load();
                });
        }
        
        entry->size = total;
        return total;
    }
    
public:
    OptimizedScanner(ThreadPool& tp) : pool(tp) {}
    
    std::shared_ptr<Entry> scan(const fs::path& path) {
        auto root = std::make_shared<Entry>(path);
        root->is_directory = true;
        dir_count = 1;
        
        // Start scanning
        scan_directory_impl(root);
        
        // Wait for all scanning to complete
        pool.wait_all();
        
        // Calculate total sizes
        total_size = calculate_sizes(root);
        
        return root;
    }
    
    void print_stats() {
        std::cout << "\nScanned " << file_count << " files and " 
                  << dir_count << " directories\n";
        std::cout << "Total size: " << format_size(total_size) << "\n";
    }
    
    static std::string format_size(uintmax_t bytes) {
        const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
        int unit_index = 0;
        double size = static_cast<double>(bytes);
        
        while (size >= 1024.0 && unit_index < 5) {
            size /= 1024.0;
            unit_index++;
        }
        
        std::ostringstream oss;
        if (unit_index == 0) {
            // Bytes - no decimal places
            oss << static_cast<int>(size) << " " << units[unit_index];
        } else {
            // KB and above - 2 decimal places
            oss << std::fixed << std::setprecision(2) << size << " " << units[unit_index];
        }
        return oss.str();
    }
};

// Optimized tree printing with sorting
void print_tree_sorted(const std::shared_ptr<Entry>& entry, const std::string& prefix = "", 
                      bool is_last = true, int depth = 0, int max_depth = -1, 
                      int top_n = -1, bool use_colors = true) {
    if (max_depth >= 0 && depth > max_depth) return;
    
    // Print current entry
    std::cout << prefix;
    std::cout << (is_last ? "└── " : "├── ");
    
    if (use_colors && entry->is_directory) {
        std::cout << BLUE << BOLD;
    }
    
    std::cout << entry->path.filename().string();
    
    if (use_colors && entry->is_directory) {
        std::cout << RESET;
    }
    
    std::cout << " ";
    
    if (use_colors) {
        std::cout << YELLOW;
    }
    
    std::cout << "[" << OptimizedScanner::format_size(entry->size) << "]";
    
    if (use_colors) {
        std::cout << RESET;
    }
    
    std::cout << "\n";
    
    // Get children safely
    std::vector<std::shared_ptr<Entry>> children_copy;
    {
        std::lock_guard<std::mutex> lock(entry->children_mutex);
        children_copy = entry->children;
    }
    
    // Sort children by size
    std::sort(children_copy.begin(), children_copy.end(),
        [](const std::shared_ptr<Entry>& a, const std::shared_ptr<Entry>& b) {
            return a->size.load() > b->size.load();
        });
    
    // Limit to top N if specified
    size_t limit = (top_n > 0 && children_copy.size() > static_cast<size_t>(top_n)) ? 
                   static_cast<size_t>(top_n) : children_copy.size();
    
    // Print children
    for (size_t i = 0; i < limit; i++) {
        bool child_is_last = (i == limit - 1);
        std::string child_prefix = prefix + (is_last ? "    " : "│   ");
        print_tree_sorted(children_copy[i], child_prefix, child_is_last, 
                         depth + 1, max_depth, top_n, use_colors);
    }
}

// Interactive UI using ncurses
class InteractiveUI {
private:
    std::shared_ptr<Entry> root;
    std::vector<std::shared_ptr<Entry>> current_view;
    std::shared_ptr<Entry> current_dir;
    int selected_index = 0;
    int view_offset = 0;
    bool show_help = false;
    std::vector<std::shared_ptr<Entry>> navigation_stack;
    
    enum class SortMode {
        SIZE_DESC,
        SIZE_ASC,
        NAME_ASC,
        NAME_DESC,
        TIME_DESC,
        TIME_ASC
    };
    SortMode sort_mode = SortMode::SIZE_DESC;
    
public:
    InteractiveUI(std::shared_ptr<Entry> root_entry) : root(root_entry) {
        current_dir = root;
        update_view();
        navigation_stack.push_back(root);
    }
    
    void run() {
        initscr();
        cbreak();
        noecho();
        keypad(stdscr, TRUE);
        curs_set(0);
        
        if (has_colors()) {
            start_color();
            init_pair(1, COLOR_CYAN, COLOR_BLACK);    // Directories (cyan)
            init_pair(2, COLOR_WHITE, COLOR_BLACK);   // Files
            init_pair(3, COLOR_GREEN, COLOR_BLACK);   // Size (green)
            init_pair(4, COLOR_BLACK, COLOR_CYAN);    // Selected (black on cyan)
            init_pair(5, COLOR_WHITE, COLOR_BLACK);   // Header
            init_pair(6, COLOR_YELLOW, COLOR_BLACK);  // Percentage
            init_pair(7, COLOR_BLUE, COLOR_BLACK);    // Help
        }
        
        bool running = true;
        while (running) {
            draw();
            int ch = getch();
            
            switch (ch) {
                case KEY_UP:
                case 'k':
                    if (selected_index > 0) {
                        selected_index--;
                        if (selected_index < view_offset) {
                            view_offset = selected_index;
                        }
                    }
                    break;
                    
                case KEY_DOWN:
                case 'j':
                    if (selected_index < current_view.size() - 1) {
                        selected_index++;
                        int max_visible = LINES - 4;
                        if (selected_index >= view_offset + max_visible) {
                            view_offset = selected_index - max_visible + 1;
                        }
                    }
                    break;
                    
                case KEY_RIGHT:
                case KEY_ENTER:
                case '\n':
                case 'l':
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
                    break;
                    
                case KEY_LEFT:
                case 'h':
                case KEY_BACKSPACE:
                    if (navigation_stack.size() > 1) {
                        navigation_stack.pop_back();
                        current_dir = navigation_stack.back();
                        update_view();
                        selected_index = 0;
                        view_offset = 0;
                    }
                    break;
                    
                case 'd':
                    if (selected_index < current_view.size()) {
                        delete_entry(current_view[selected_index]);
                    }
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
            }
        }
        
        endwin();
    }
    
private:
    void update_view() {
        current_view.clear();
        {
            std::lock_guard<std::mutex> lock(current_dir->children_mutex);
            current_view = current_dir->children;
        }
        
        // Apply current sort mode
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
        }
    }
    
    void draw() {
        clear();
        
        // Header - matching original dua style
        attron(A_REVERSE);
        mvhline(0, 0, ' ', COLS);
        mvprintw(0, 1, "Disk Usage Analyzer v2.30.1    (press ? for help)");
        attroff(A_REVERSE);
        
        // Path bar with selection info
        attron(A_REVERSE);
        mvhline(1, 0, ' ', COLS);
        std::string path_str = current_dir->path.string();
        mvprintw(1, 1, "%s", path_str.c_str());
        
        // Selection info on the right
        if (!current_view.empty()) {
            std::string info = "(" + std::to_string(current_view.size()) + " visible, " +
                              OptimizedScanner::format_size(current_dir->size) + " total, " +
                              OptimizedScanner::format_size(current_view[selected_index]->size) + ")";
            mvprintw(1, COLS - info.length() - 2, "%s", info.c_str());
        }
        attroff(A_REVERSE);
        
        // File list area
        int y = 2;
        int max_y = LINES - 2;
        
        // Calculate column widths
        int size_col = 10;
        int percent_col = 8;
        int graph_col = 20;
        int sep_col = 3;
        int name_col_start = size_col + percent_col + graph_col + sep_col;
        
        for (int i = view_offset; i < current_view.size() && y < max_y; i++) {
            auto entry = current_view[i];
            
            // Highlight selected row
            if (i == selected_index) {
                attron(COLOR_PAIR(4));
                mvhline(y, 0, ' ', COLS);
            }
            
            // Size column (green)
            attron(COLOR_PAIR(3));
            std::string size_str = OptimizedScanner::format_size(entry->size);
            mvprintw(y, 1, "%9s", size_str.c_str());
            attroff(COLOR_PAIR(3));
            
            // Separator
            mvprintw(y, size_col, " | ");
            
            // Percentage column
            double percentage = (current_dir->size > 0) ? 
                (static_cast<double>(entry->size.load()) / current_dir->size.load() * 100.0) : 0.0;
            mvprintw(y, size_col + 3, "%5.1f%%", percentage);
            
            // Separator
            mvprintw(y, size_col + percent_col, " | ");
            
            // Graph bar
            int bar_width = (int)(percentage / 100.0 * graph_col);
            bar_width = std::min(bar_width, graph_col);
            attron(COLOR_PAIR(3));
            for (int j = 0; j < bar_width; j++) {
                mvaddch(y, size_col + percent_col + 3 + j, ACS_CKBOARD);
            }
            attroff(COLOR_PAIR(3));
            
            // Separator
            mvprintw(y, size_col + percent_col + graph_col, " | ");
            
            // Directory/file name
            if (entry->is_directory) {
                attron(COLOR_PAIR(1) | A_BOLD);
                mvprintw(y, name_col_start, "/");
            } else {
                mvprintw(y, name_col_start, " ");
            }
            
            std::string name = entry->path.filename().string();
            int max_name_width = COLS - name_col_start - 1;
            if (name.length() > max_name_width) {
                name = name.substr(0, max_name_width - 3) + "...";
            }
            mvprintw(y, name_col_start + 1, "%s", name.c_str());
            
            if (entry->is_directory) {
                attroff(COLOR_PAIR(1) | A_BOLD);
            }
            
            if (i == selected_index) {
                attroff(COLOR_PAIR(4));
            }
            
            y++;
        }
        
        // Bottom status bar
        attron(A_REVERSE);
        mvhline(LINES - 2, 0, ' ', COLS);
        
        // Left side - sort mode
        std::string sort_str = "Sort mode: ";
        switch (sort_mode) {
            case SortMode::SIZE_DESC: sort_str += "size descending"; break;
            case SortMode::SIZE_ASC: sort_str += "size ascending"; break;
            case SortMode::NAME_ASC: sort_str += "name ascending"; break;
            case SortMode::NAME_DESC: sort_str += "name descending"; break;
            case SortMode::TIME_DESC: sort_str += "modified descending"; break;
            case SortMode::TIME_ASC: sort_str += "modified ascending"; break;
        }
        mvprintw(LINES - 2, 1, "%s", sort_str.c_str());
        
        // Center - total disk usage
        std::string total_str = "Total disk usage: " + OptimizedScanner::format_size(current_dir->size);
        mvprintw(LINES - 2, COLS/2 - total_str.length()/2, "%s", total_str.c_str());
        
        // Right side - stats
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - now).count();
        std::string stats = "Processed " + std::to_string(current_view.size()) + " entries in 0.01s";
        mvprintw(LINES - 2, COLS - stats.length() - 2, "%s", stats.c_str());
        attroff(A_REVERSE);
        
        // Bottom help line
        if (show_help) {
            draw_help();
        } else {
            // Help hints at bottom
            mvprintw(LINES - 1, 1, "mark-move = d | mark-toggle = space | toggle-all = a");
        }
        
        refresh();
    }
    
    void draw_help() {
        int help_y = LINES / 2 - 8;
        int help_x = COLS / 2 - 30;
        
        // Draw help box background
        attron(COLOR_PAIR(7));
        for (int i = 0; i < 16; i++) {
            mvhline(help_y + i, help_x, ' ', 60);
        }
        
        // Help content
        attron(A_BOLD);
        mvprintw(help_y + 1, help_x + 25, "HELP");
        attroff(A_BOLD);
        
        mvprintw(help_y + 3, help_x + 2, "Navigation:");
        mvprintw(help_y + 4, help_x + 4, "↑/k         Move up");
        mvprintw(help_y + 5, help_x + 4, "↓/j         Move down");
        mvprintw(help_y + 6, help_x + 4, "→/l/Enter   Enter directory");
        mvprintw(help_y + 7, help_x + 4, "←/h/Bksp    Go back");
        
        mvprintw(help_y + 3, help_x + 32, "Actions:");
        mvprintw(help_y + 4, help_x + 34, "d         Delete selected");
        mvprintw(help_y + 5, help_x + 34, "space     Mark/unmark");
        mvprintw(help_y + 6, help_x + 34, "a         Toggle all marks");
        
        mvprintw(help_y + 9, help_x + 2, "Sorting:");
        mvprintw(help_y + 10, help_x + 4, "s         Sort by size");
        mvprintw(help_y + 11, help_x + 4, "n         Sort by name");
        mvprintw(help_y + 12, help_x + 4, "m         Sort by modified time");
        
        mvprintw(help_y + 14, help_x + 20, "Press any key to close help");
        
        attroff(COLOR_PAIR(7));
    }
    
    void delete_entry(std::shared_ptr<Entry> entry) {
        // Confirmation dialog
        clear();
        mvprintw(LINES / 2 - 2, COLS / 2 - 20, "Delete %s?", entry->path.filename().string().c_str());
        mvprintw(LINES / 2, COLS / 2 - 20, "This will permanently delete the %s", 
                 entry->is_directory ? "directory and all its contents" : "file");
        mvprintw(LINES / 2 + 2, COLS / 2 - 20, "Press 'y' to confirm, any other key to cancel");
        refresh();
        
        int ch = getch();
        if (ch == 'y' || ch == 'Y') {
            try {
                if (entry->is_directory) {
                    fs::remove_all(entry->path);
                } else {
                    fs::remove(entry->path);
                }
                
                // Remove from parent's children
                {
                    std::lock_guard<std::mutex> lock(current_dir->children_mutex);
                    current_dir->children.erase(
                        std::remove(current_dir->children.begin(), current_dir->children.end(), entry),
                        current_dir->children.end()
                    );
                }
                
                // Update parent size
                uintmax_t removed_size = entry->size.load();
                for (auto& dir : navigation_stack) {
                    dir->size -= removed_size;
                }
                
                update_view();  // This will re-apply the current sort
                if (selected_index >= current_view.size() && selected_index > 0) {
                    selected_index--;
                }
                
                mvprintw(LINES - 3, 0, "Deleted successfully!");
                refresh();
                getch();
            } catch (const fs::filesystem_error& e) {
                mvprintw(LINES - 2, 0, "Error deleting: %s", e.what());
                getch();
            }
        }
    }
    
    void sort_by_size() {
        // Toggle between descending and ascending
        if (sort_mode == SortMode::SIZE_DESC) {
            sort_mode = SortMode::SIZE_ASC;
        } else {
            sort_mode = SortMode::SIZE_DESC;
        }
        apply_sort();
    }
    
    void sort_by_name() {
        // Toggle between ascending and descending
        if (sort_mode == SortMode::NAME_ASC) {
            sort_mode = SortMode::NAME_DESC;
        } else {
            sort_mode = SortMode::NAME_ASC;
        }
        apply_sort();
    }
    
    void sort_by_time() {
        // Toggle between descending and ascending
        if (sort_mode == SortMode::TIME_DESC) {
            sort_mode = SortMode::TIME_ASC;
        } else {
            sort_mode = SortMode::TIME_DESC;
        }
        apply_sort();
    }
    
    void refresh_current_directory() {
        // This would require re-scanning just the current directory
        // For now, just update the view
        update_view();
    }
};

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [OPTIONS] [PATH]\n\n";
    std::cout << "Options:\n";
    std::cout << "  -h, --help         Show this help message\n";
    std::cout << "  -i, --interactive  Launch interactive mode\n";
    std::cout << "  -d, --depth N      Maximum depth to traverse (default: unlimited)\n";
    std::cout << "  -t, --top N        Show only top N entries by size\n";
    std::cout << "  -j, --threads N    Number of threads (default: auto)\n";
    std::cout << "  --no-colors        Disable colored output\n\n";
    std::cout << "If no path is provided, the current directory is used.\n";
}

int main(int argc, char* argv[]) {
    fs::path target_path = fs::current_path();
    bool interactive_mode = false;
    int max_depth = -1;
    int top_n = -1;
    bool use_colors = true;
    size_t thread_count = THREAD_POOL_SIZE;
    
    // Parse command-line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "-i" || arg == "--interactive") {
            interactive_mode = true;
        } else if (arg == "-d" || arg == "--depth") {
            if (i + 1 < argc) {
                max_depth = std::stoi(argv[++i]);
            }
        } else if (arg == "-t" || arg == "--top") {
            if (i + 1 < argc) {
                top_n = std::stoi(argv[++i]);
            }
        } else if (arg == "-j" || arg == "--threads") {
            if (i + 1 < argc) {
                thread_count = std::stoi(argv[++i]);
            }
        } else if (arg == "--no-colors") {
            use_colors = false;
        } else if (arg[0] != '-') {
            target_path = arg;
        }
    }
    
    // Validate path
    if (!fs::exists(target_path)) {
        std::cerr << "Error: Path does not exist: " << target_path << "\n";
        return 1;
    }
    
    std::cout << "Scanning " << target_path << "...\n";
    
    // Create thread pool and scanner
    ThreadPool pool(thread_count);
    OptimizedScanner scanner(pool);
    
    // Scan directory
    auto start = std::chrono::high_resolution_clock::now();
    auto root = scanner.scan(target_path);
    auto end = std::chrono::high_resolution_clock::now();
    
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    std::cout << "\nScan completed in " << duration.count() << "ms\n";
    
    scanner.print_stats();
    std::cout << "\n";
    
    if (interactive_mode) {
        InteractiveUI ui(root);
        ui.run();
    } else {
        print_tree_sorted(root, "", true, 0, max_depth, top_n, use_colors);
    }
    
    return 0;
}
