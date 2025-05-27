// dua_core.cpp - Core functionality implementation
#include "dua_core.h"

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
        return st.st_blocks * 512;
    }
#endif
    const uintmax_t block_size = 4096;
    return ((file_size + block_size - 1) / block_size) * block_size;
}

// Glob pattern matching
bool glob_match(const std::string& pattern, const std::string& text) {
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

// Helper function to shorten paths for display
std::string shorten_path(const std::string& path, size_t max_length) {
    if (path.length() <= max_length) {
        return path;
    }
    
    const size_t prefix_len = 30;
    const size_t suffix_len = 30;
    const std::string ellipsis = "...";
    
    if (path.length() <= prefix_len + suffix_len + ellipsis.length()) {
        return path;
    }
    
    return path.substr(0, prefix_len) + ellipsis + 
           path.substr(path.length() - suffix_len);
}

// WorkStealingThreadPool implementation
bool WorkStealingThreadPool::try_steal(size_t thief_id, std::function<void()>& task) {
    const size_t actual_threads = queues.size();
    for (size_t i = 1; i < actual_threads; ++i) {
        size_t victim_id = (thief_id + i) % actual_threads;
        auto& victim_queue = queues[victim_id];
        
        if (victim_queue->size.load() > 0) {
            std::unique_lock<std::mutex> lock(victim_queue->mutex, std::try_to_lock);
            if (lock.owns_lock() && !victim_queue->tasks.empty()) {
                task = std::move(victim_queue->tasks.back());
                victim_queue->tasks.pop_back();
                victim_queue->size--;
                return true;
            }
        }
    }
    return false;
}

void WorkStealingThreadPool::worker_thread(size_t id) {
    auto& my_queue = queues[id];
    
    while (!stop) {
        std::function<void()> task;
        
        {
            std::unique_lock<std::mutex> lock(my_queue->mutex);
            if (!my_queue->tasks.empty()) {
                task = std::move(my_queue->tasks.front());
                my_queue->tasks.pop_front();
                my_queue->size--;
            }
        }
        
        if (!task && !try_steal(id, task)) {
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

WorkStealingThreadPool::WorkStealingThreadPool(size_t threads) {
    num_threads = threads;
    if (num_threads == 0) {
        num_threads = std::thread::hardware_concurrency();
        if (num_threads == 0) num_threads = 4;
    }
    
#ifdef __APPLE__
    num_threads = std::min(num_threads, size_t(3));
#endif
    
    queues.reserve(num_threads);
    for (size_t i = 0; i < num_threads; ++i) {
        queues.emplace_back(std::make_unique<WorkQueue>());
    }
    
    workers.reserve(num_threads);
    for (size_t i = 0; i < num_threads; ++i) {
        workers.emplace_back(&WorkStealingThreadPool::worker_thread, this, i);
    }
}

WorkStealingThreadPool::~WorkStealingThreadPool() {
    stop = true;
    work_available.notify_all();
    for (auto& worker : workers) {
        worker.join();
    }
}

void WorkStealingThreadPool::wait_all() {
    while (total_tasks.load() > 0 || active_workers.load() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// InodeKey implementation
bool OptimizedScanner::InodeKey::operator==(const InodeKey& other) const {
    return device == other.device && inode == other.inode;
}

std::size_t OptimizedScanner::InodeKeyHash::operator()(const InodeKey& k) const {
    return std::hash<dev_t>()(k.device) ^ (std::hash<ino_t>()(k.inode) << 1);
}

// OptimizedScanner implementation
OptimizedScanner::OptimizedScanner(WorkStealingThreadPool& tp, Config& cfg) 
    : pool(tp), config(cfg), progress_throttle(std::chrono::milliseconds(100)) {
    start_time = std::chrono::steady_clock::now();
}

bool OptimizedScanner::should_count_entry(const Entry& entry) {
    if (!config.count_hard_links && entry.hard_link_count > 1) {
        std::lock_guard<std::mutex> lock(inode_mutex);
        InodeKey key{entry.device_id, entry.inode};
        auto it = inode_map.find(key);
        if (it != inode_map.end()) {
            return false;
        }
        inode_map[key] = 1;
    }
    return true;
}

bool OptimizedScanner::should_ignore_directory(const fs::path& path) {
    fs::path canonical_path;
    try {
        canonical_path = fs::canonical(path);
    } catch (...) {
        canonical_path = path;
    }
    
    {
        std::lock_guard<std::mutex> lock(visited_mutex);
        if (!visited_dirs.insert(canonical_path.string()).second) {
            return true;
        }
    }
    
    return config.ignore_dirs.find(canonical_path) != config.ignore_dirs.end();
}

void OptimizedScanner::update_progress() const {
    if (config.show_progress && progress_throttle.should_update()) {
        size_t current_entries = entries_traversed.load();
        size_t skipped = skipped_entries.load();
        std::string path_display;
        
        {
            std::lock_guard<std::mutex> lock(current_path_mutex);
            path_display = current_path;
        }
        
        std::string shortened = shorten_path(path_display);
        std::cerr << "\rEnumerating " << current_entries << " items";
        if (skipped > 0) {
            std::cerr << " (skipped " << skipped << ")";
        }
        std::cerr << " - " << shortened << std::flush;
    }
}

bool OptimizedScanner::try_iterate_directory(const fs::path& dir_path, 
                          std::vector<fs::directory_entry>& entries) {
    try {
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
        
        if (future.wait_for(FS_TIMEOUT) == std::future_status::ready) {
            return future.get();
        } else {
            skipped_entries++;
            return false;
        }
    } catch (...) {
        return false;
    }
}

void OptimizedScanner::scan_directory_batch(std::shared_ptr<Entry> parent, 
                        const std::vector<fs::directory_entry>& batch,
                        dev_t root_device) {
    for (const auto& item : batch) {
        try {
            auto child = std::make_shared<Entry>(item.path());
            
            {
                std::lock_guard<std::mutex> lock(current_path_mutex);
                current_path = item.path().string();
            }
            
            if (!child->is_symlink && config.stay_on_filesystem && 
                child->device_id != root_device) {
                continue;
            }
            
            entries_traversed++;
            update_progress();
            
            if (child->is_symlink) {
                symlink_count++;
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

void OptimizedScanner::scan_directory_impl(std::shared_ptr<Entry> entry, dev_t root_device) {
    if (entry->is_symlink || should_ignore_directory(entry->path)) {
        return;
    }
    
    {
        std::lock_guard<std::mutex> lock(current_path_mutex);
        current_path = entry->path.string();
    }
    
    std::vector<fs::directory_entry> entries;
    entries.reserve(BATCH_SIZE * 2);
    
    if (!try_iterate_directory(entry->path, entries)) {
        io_errors++;
        return;
    }
    
    std::vector<fs::directory_entry> batch;
    batch.reserve(BATCH_SIZE);
    
    for (const auto& item : entries) {
        batch.push_back(item);
        
        if (batch.size() >= BATCH_SIZE) {
            scan_directory_batch(entry, batch, root_device);
            batch.clear();
        }
    }
    
    if (!batch.empty()) {
        scan_directory_batch(entry, batch, root_device);
    }
}

uintmax_t OptimizedScanner::calculate_sizes(std::shared_ptr<Entry> entry) {
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

std::vector<std::shared_ptr<Entry>> OptimizedScanner::scan(const std::vector<fs::path>& paths) {
    std::vector<std::shared_ptr<Entry>> roots;
    
    for (const auto& path : paths) {
        auto root = std::make_shared<Entry>(path);
        root->is_directory = fs::is_directory(path);
        
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
    
    if (config.show_progress) {
        progress_throttle.clear_line();
    }
    
    for (auto& root : roots) {
        total_size += calculate_sizes(root);
    }
    
    return roots;
}

void OptimizedScanner::print_stats() {
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

// Tree printing function
void print_tree_sorted(const std::shared_ptr<Entry>& entry, const Config& config,
                      const std::string& prefix, bool is_last, 
                      int depth, std::vector<std::shared_ptr<Entry>>* siblings) {
    (void)siblings;  // Suppress unused parameter warning
    if (config.max_depth >= 0 && depth > config.max_depth) return;
    
    std::cout << prefix;
    
    if (depth > 0) {
        std::cout << (is_last ? "└── " : "├── ");
    }
    
    if (!config.no_colors) {
        if (entry->is_symlink) {
            std::cout << MAGENTA;
        } else if (entry->is_directory) {
            std::cout << BLUE << BOLD;
        }
    }
    
    std::string name = entry->path.filename().string();
    if (name.empty() && depth == 0) {
        name = entry->path.string();
    }
    
    std::cout << name;
    
    if (entry->is_symlink) {
        std::cout << " -> " << entry->symlink_target.string();
    }
    
    if (!config.no_colors && (entry->is_symlink || entry->is_directory)) {
        std::cout << RESET;
    }
    
    std::cout << " ";
    
    if (!config.no_colors) {
        std::cout << YELLOW;
    }
    
    std::cout << "[" << format_size(entry->size, config.format) << "]";
    
    if (!config.no_colors) {
        std::cout << RESET;
    }
    
    std::cout << "\n";
    
    if (entry->is_directory && !entry->is_symlink) {
        std::vector<std::shared_ptr<Entry>> children_copy;
        {
            std::lock_guard<std::mutex> lock(entry->children_mutex);
            children_copy = entry->children;
        }
        
        std::sort(children_copy.begin(), children_copy.end(),
            [](const std::shared_ptr<Entry>& a, const std::shared_ptr<Entry>& b) {
                return a->size.load() > b->size.load();
            });
        
        size_t limit = children_copy.size();
        if (config.top_n > 0 && limit > static_cast<size_t>(config.top_n)) {
            limit = static_cast<size_t>(config.top_n);
        }
        
        for (size_t i = 0; i < limit; i++) {
            bool child_is_last = (i == limit - 1);
            std::string child_prefix = prefix + (is_last ? "    " : "│   ");
            
            print_tree_sorted(children_copy[i], config, child_prefix, child_is_last, 
                             depth + 1, &children_copy);
        }
        
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
