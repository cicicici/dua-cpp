// dua_enhanced.cpp - Enhanced Disk Usage Analyzer main program
// Refactored with modular architecture

#include "dua_core.h"
#include "dua_ui.h"

// Define color constants
const std::string RESET = "\033[0m";
const std::string RED = "\033[31m";
const std::string GREEN = "\033[32m";
const std::string YELLOW = "\033[33m";
const std::string BLUE = "\033[34m";
const std::string MAGENTA = "\033[35m";
const std::string CYAN = "\033[36m";
const std::string BOLD = "\033[1m";
const std::string GRAY = "\033[90m";
const std::string CLEAR_LINE = "\033[2K\r";

// Function declarations
void aggregate_mode(Config& config);
void print_usage(const char* program_name);
void print_version();

// Entry implementation
Entry::Entry(const fs::path& p) : path(p) {
    children.reserve(PREALLOCATE_ENTRIES);
    try {
        if (fs::exists(path)) {
            auto status = fs::symlink_status(path);
            is_symlink = fs::is_symlink(status);
            
            if (is_symlink) {
                try {
                    symlink_target = fs::read_symlink(path);
                } catch (...) {
                    symlink_target = fs::path("[unreadable]");
                }
                last_modified = fs::file_time_type{};
            } else {
                last_modified = fs::last_write_time(path);
                
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

// ProgressThrottle implementation
ProgressThrottle::ProgressThrottle(std::chrono::milliseconds interval) 
    : update_interval(interval) {
    is_tty = isatty(fileno(stderr));
    last_update = std::chrono::steady_clock::now();
}

bool ProgressThrottle::should_update() const {
    if (!is_tty) return false;
    
    std::lock_guard<std::mutex> lock(mutex);
    auto now = std::chrono::steady_clock::now();
    if (now - last_update >= update_interval) {
        last_update = now;
        return true;
    }
    return false;
}

void ProgressThrottle::clear_line() const {
    if (is_tty) {
        std::cerr << CLEAR_LINE << std::flush;
    }
}

// Aggregate mode implementation
void aggregate_mode(Config& config) {
    WorkStealingThreadPool pool(config.thread_count);
    OptimizedScanner scanner(pool, config);
    
    auto roots = scanner.scan(config.paths);
    
    if (config.tree_mode) {
        std::cout << "\n";
        
        if (roots.size() == 1) {
            print_tree_sorted(roots[0], config);
        } else {
            auto virtual_root = std::make_shared<Entry>("[Total]");
            virtual_root->is_directory = true;
            
            for (auto& root : roots) {
                virtual_root->children.push_back(root);
                virtual_root->size += root->size.load();
                virtual_root->entry_count += root->entry_count.load();
            }
            
            std::sort(virtual_root->children.begin(), virtual_root->children.end(),
                [](const std::shared_ptr<Entry>& a, const std::shared_ptr<Entry>& b) {
                    return a->size.load() > b->size.load();
                });
            
            print_tree_sorted(virtual_root, config);
        }
        
        std::cout << "\n";
    } else {
        std::sort(roots.begin(), roots.end(),
            [](const std::shared_ptr<Entry>& a, const std::shared_ptr<Entry>& b) {
                return a->size.load() < b->size.load();
            });
        
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
            
            if (root->is_symlink) {
                std::cout << " -> " << root->symlink_target.string();
            }
            
            if (!config.no_colors && (root->is_symlink || root->is_directory)) {
                std::cout << RESET;
            }
            
            std::cout << "\n";
        }
        
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
    std::cout << "dua " << DUA_VERSION << " - Disk Usage Analyzer\n";
    std::cout << "Usage: " << program_name << " [SUBCOMMAND] [OPTIONS] [PATH...]\n\n";
    std::cout << "A tool to conveniently learn about disk usage, fast!\n\n";
    std::cout << "Subcommands:\n";
    std::cout << "  i, interactive    Launch interactive mode\n";
    std::cout << "  a, aggregate      Aggregate disk usage (default)\n\n";
    std::cout << "Options:\n";
    std::cout << "  -h, --help              Show this help message\n";
    std::cout << "  -v, --version           Show version information\n";
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

void print_version() {
    std::cout << "dua " << DUA_VERSION << "\n";
    std::cout << "Build date: " << BUILD_DATE << "\n";
    std::cout << "Git hash: " << GIT_HASH << "\n";
}

int main(int argc, char* argv[]) {
    Config config;
    std::string subcommand;
    
    std::vector<std::string> args(argv + 1, argv + argc);
    
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& arg = args[i];
        
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "-v" || arg == "--version") {
            print_version();
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
    
    if (config.paths.empty()) {
        config.paths.push_back(".");
    }
    
    for (const auto& path : config.paths) {
        if (!fs::exists(path)) {
            std::cerr << "Error: Path does not exist: " << path << "\n";
            return 1;
        }
    }
    
    if (subcommand.empty() && !config.tree_mode && isatty(fileno(stdout))) {
        config.interactive_mode = true;
    }
    
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
