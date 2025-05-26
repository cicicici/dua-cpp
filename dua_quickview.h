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

class QuickView {
private:
    static constexpr size_t MAX_PREVIEW_LINES = 100;
    static constexpr size_t MAX_LINE_LENGTH = 256;
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
    
public:
    void switch_to_tab(int tab_number);
    MarkPaneTab get_current_tab() const { return current_tab; }
    bool is_quickview_active() const { return quickview_active; }
    
    void activate_quickview(const fs::path& path);
    void deactivate_quickview();
    
    const PreviewContent& get_cached_preview() const { return cached_preview; }
    void update_preview(const fs::path& path);
};

#endif // DUA_QUICKVIEW_H