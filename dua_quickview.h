// dua_quickview.h - Quick file preview functionality
#ifndef DUA_QUICKVIEW_H
#define DUA_QUICKVIEW_H

#include <string>
#include <vector>
#include <memory>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <sys/stat.h>

namespace fs = std::filesystem;

// Preview types supported
enum class PreviewType {
    TEXT,
    BINARY,
    DIRECTORY,
    IMAGE,
    ARCHIVE,
    EMPTY,
    ERROR
};

// Structure to hold preview content
struct PreviewContent {
    PreviewType type;
    std::vector<std::string> lines;
    std::string error_message;
    size_t total_lines;
    size_t file_size;
    std::string mime_type;
};

// Scrollable view state for quick view
struct ScrollableView {
    size_t cursor_x = 0;        // Horizontal cursor position
    size_t cursor_y = 0;        // Vertical cursor position
    size_t view_offset_x = 0;   // Horizontal scroll offset
    size_t view_offset_y = 0;   // Vertical scroll offset
    size_t max_line_length = 0; // Maximum line length in content
    
    // Window dimensions
    size_t window_width = 0;
    size_t window_height = 0;
    
    // Content dimensions
    size_t content_width = 0;
    size_t content_height = 0;
    
    // Store line lengths for proper cursor movement
    std::vector<size_t> line_lengths;
    
    // Search functionality
    struct SearchMatch {
        size_t line;
        size_t column;
    };
    std::vector<SearchMatch> search_matches;
    std::string search_pattern;
    size_t current_match_index = 0;
    bool search_active = false;
    
    // Navigation methods
    void move_up();
    void move_down();
    void move_left();
    void move_right();
    void page_up();
    void page_down();
    void move_home();
    void move_end();
    void move_line_start();
    void move_line_end();
    
    // Update window size
    void update_window_size(size_t width, size_t height);
    
    // Update content info
    void update_content_info(const std::vector<std::string>& lines);
    
    // Get visible range
    size_t get_visible_start_y() const { return view_offset_y; }
    size_t get_visible_end_y() const { return std::min(view_offset_y + window_height, content_height); }
    size_t get_visible_start_x() const { return view_offset_x; }
    size_t get_visible_end_x() const { return view_offset_x + window_width; }
    
    // Reset state
    void reset();
    
    // Search methods
    void start_search();
    void end_search();
    void perform_search(const std::vector<std::string>& lines);
    void next_match();
    void prev_match();
    void move_to_match(size_t match_index);
    bool has_matches() const { return !search_matches.empty(); }
    size_t get_match_count() const { return search_matches.size(); }
    size_t get_current_match_index() const { return current_match_index; }
};

class QuickView {
private:
    static constexpr size_t MAX_PREVIEW_LINES = 10000;  // Much larger for scrollable view
    static constexpr size_t MAX_LINE_LENGTH = 4096;     // Much longer lines supported
    static constexpr size_t MAX_FILE_SIZE = 10 * 1024 * 1024; // 10MB
    
    // File type detection
    static PreviewType detect_file_type(const fs::path& path);
    static bool is_text_file(const fs::path& path);
    
    // Preview generators
    static PreviewContent preview_text_file(const fs::path& path);
    static PreviewContent preview_directory(const fs::path& path);
    static PreviewContent preview_binary_file(const fs::path& path);
    static PreviewContent preview_image_file(const fs::path& path);
    static PreviewContent preview_archive_file(const fs::path& path);
    
    // Helper functions
    static std::string format_size(uintmax_t size);
    static std::string format_permissions(const fs::path& path);
    static std::string truncate_line(const std::string& line, size_t max_length);
    static bool is_binary_data(const char* data, size_t size);

public:
    // Main preview function
    static PreviewContent generate_preview(const fs::path& path);
    
    // Format preview for display
    static std::vector<std::string> format_preview(const PreviewContent& content, 
                                                   size_t width, size_t height);
};

// Tab manager for mark pane
enum class MarkPaneTab {
    QUICKVIEW = 0,
    MARKED_FILES = 1
};

class TabManager {
private:
    MarkPaneTab current_tab = MarkPaneTab::MARKED_FILES;
    bool quickview_active = false;
    fs::path current_preview_path;
    PreviewContent cached_preview;
    ScrollableView scroll_view;
    
public:
    void switch_to_tab(int tab_number);
    MarkPaneTab get_current_tab() const { return current_tab; }
    bool is_quickview_active() const { return quickview_active; }
    
    void activate_quickview(const fs::path& path);
    void deactivate_quickview();
    
    const PreviewContent& get_cached_preview() const { return cached_preview; }
    ScrollableView& get_scroll_view() { return scroll_view; }
    const ScrollableView& get_scroll_view() const { return scroll_view; }
    void update_preview(const fs::path& path);
};

#endif // DUA_QUICKVIEW_H