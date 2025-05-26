// dua_ui.h - UI functionality for Disk Usage Analyzer
#ifndef DUA_UI_H
#define DUA_UI_H

#include "dua_core.h"
#include "dua_quickview.h"
#include <ncurses.h>

// Forward declarations
class MarkPane;
class InteractiveUI;

// Sorting modes
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

// Focused pane enum
enum class FocusedPane {
    Main,
    Mark
};

// Cache for rendered lines to detect changes
struct LineCache {
    std::string content;
    int attributes;
    bool is_selected;
    
    bool operator!=(const LineCache& other) const;
};

// Cached formatted strings for entries
struct CachedEntry {
    std::string formatted_size;
    std::string formatted_time;
    std::string formatted_name;
    double percentage;
    bool needs_update = true;
};

// Mark Pane - provides a focused view of all marked items with tab support
class MarkPane {
private:
    std::vector<std::shared_ptr<Entry>> marked_items;
    std::vector<std::string> marked_paths;
    std::vector<uintmax_t> marked_sizes;
    size_t selected_index = 0;
    size_t view_offset = 0;
    bool has_focus = false;
    Config& config;
    
    // Tab support
    TabManager tab_manager;
    PreviewContent current_preview;
    
    void collect_marked_recursive(std::shared_ptr<Entry> entry);
    void adjust_view_offset();
    void draw_scrollbar(WINDOW* win, int height, size_t offset, size_t total, int visible);
    void draw_tabs(WINDOW* win, int width);
    void draw_quickview(WINDOW* win, int height, int width);
    void draw_marked_files(WINDOW* win, int height, int width);
    
public:
    MarkPane(Config& cfg);
    
    void set_focus(bool focus);
    bool is_focused() const;
    bool is_empty() const;
    size_t count() const;
    uintmax_t total_size() const;
    
    void update_marked_items(const std::vector<std::shared_ptr<Entry>>& roots);
    void navigate_up();
    void navigate_down();
    void navigate_page_up();
    void navigate_page_down();
    void navigate_home();
    void navigate_end();
    void remove_selected();
    void remove_all();
    std::vector<std::shared_ptr<Entry>> get_all_marked() const;
    void draw(WINDOW* win, int height, int width);
    
    // Tab and quickview support
    void switch_tab(int tab_number);
    void activate_quickview(const fs::path& path);
    void deactivate_quickview();
    bool is_quickview_active() const;
    MarkPaneTab get_current_tab() const;
};

// Interactive UI class
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
    
    // Mark pane
    MarkPane mark_pane;
    WINDOW* main_win = nullptr;
    WINDOW* mark_win = nullptr;
    
    FocusedPane focused_pane = FocusedPane::Main;
    
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
    std::unordered_map<std::shared_ptr<Entry>, CachedEntry> format_cache;
    
    SortMode sort_mode = SortMode::SIZE_DESC;
    
    // Window management
    void update_window_layout();
    void switch_focus();
    void check_mark_pane_visibility();
    
    // Navigation
    void navigate_up();
    void navigate_down();
    void enter_directory();
    void exit_directory();
    void apply_movement(int delta);
    
    // Marking
    void toggle_mark();
    void toggle_all_marks();
    bool has_marked_items();
    bool has_any_marked_items();
    bool has_marked_recursive(std::shared_ptr<Entry> root);
    size_t count_marked_items();
    void count_marked_recursive(std::shared_ptr<Entry> root, size_t& count);
    uintmax_t calculate_marked_size();
    void calculate_marked_size_recursive(std::shared_ptr<Entry> root, uintmax_t& total);
    void collect_marked_entries(std::shared_ptr<Entry> root, 
                               std::vector<std::shared_ptr<Entry>>& marked);
    void delete_marked_entries();
    void delete_marked_from_pane();
    void remove_from_parent(std::shared_ptr<Entry> entry);
    
    // Sorting
    void apply_sort();
    void sort_by_size();
    void sort_by_name();
    void sort_by_time();
    void sort_by_count();
    
    // Searching
    void start_glob_search();
    void handle_glob_search(int ch);
    void perform_glob_search();
    void search_entries(std::shared_ptr<Entry> root, const std::string& pattern, 
                       std::vector<std::shared_ptr<Entry>>& matches);
    
    // Refreshing
    void refresh_selected();
    void refresh_all();
    
    // Window management
    void handle_resize();
    void update_view();
    
    // Drawing
    void draw_full();
    void draw_differential();
    void draw_entry_line(size_t index, int y, bool force_redraw, WINDOW* win, int win_width);
    void update_format_cache(std::shared_ptr<Entry> entry, CachedEntry& cached, int win_width);
    void update_status_line(WINDOW* win, int height, int width);
    void draw_help(WINDOW* win);
    
    // Input handling
    bool handle_key(int ch);
    bool handle_mark_pane_key(int ch);
    
    // System operations
    void open_selected();
    void print_marked_paths();
    
public:
    InteractiveUI(std::vector<std::shared_ptr<Entry>> root_entries, Config& cfg);
    ~InteractiveUI();
    
    void run();
};

#endif // DUA_UI_H
