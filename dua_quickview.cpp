// dua_quickview.cpp - Quick file preview implementation
#include "dua_quickview.h"
#include <iomanip>
#include <cstring>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>

// Helper to format file size
std::string QuickView::format_size(uintmax_t size) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_index = 0;
    double dsize = static_cast<double>(size);
    
    while (dsize >= 1024.0 && unit_index < 4) {
        dsize /= 1024.0;
        unit_index++;
    }
    
    std::ostringstream oss;
    if (unit_index == 0) {
        oss << size << " " << units[unit_index];
    } else {
        oss << std::fixed << std::setprecision(2) << dsize << " " << units[unit_index];
    }
    return oss.str();
}

// Helper to format file permissions
std::string QuickView::format_permissions(const fs::path& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return "?????????";
    }
    
    std::string perms;
    perms += (S_ISDIR(st.st_mode)) ? 'd' : '-';
    perms += (st.st_mode & S_IRUSR) ? 'r' : '-';
    perms += (st.st_mode & S_IWUSR) ? 'w' : '-';
    perms += (st.st_mode & S_IXUSR) ? 'x' : '-';
    perms += (st.st_mode & S_IRGRP) ? 'r' : '-';
    perms += (st.st_mode & S_IWGRP) ? 'w' : '-';
    perms += (st.st_mode & S_IXGRP) ? 'x' : '-';
    perms += (st.st_mode & S_IROTH) ? 'r' : '-';
    perms += (st.st_mode & S_IWOTH) ? 'w' : '-';
    perms += (st.st_mode & S_IXOTH) ? 'x' : '-';
    
    return perms;
}

// Truncate long lines for display
std::string QuickView::truncate_line(const std::string& line, size_t max_length) {
    if (line.length() <= max_length) {
        return line;
    }
    return line.substr(0, max_length - 3) + "...";
}

// Check if data contains binary content
bool QuickView::is_binary_data(const char* data, size_t size) {
    size_t check_size = std::min(size, size_t(8192));
    for (size_t i = 0; i < check_size; i++) {
        unsigned char c = static_cast<unsigned char>(data[i]);
        if (c == 0 || (c < 32 && c != '\t' && c != '\n' && c != '\r')) {
            return true;
        }
    }
    return false;
}

// Simple file type detection
bool QuickView::is_text_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    
    char buffer[8192];
    file.read(buffer, sizeof(buffer));
    size_t read_size = file.gcount();
    
    return !is_binary_data(buffer, read_size);
}

// Detect file type
PreviewType QuickView::detect_file_type(const fs::path& path) {
    if (!fs::exists(path)) {
        return PreviewType::ERROR;
    }
    
    if (fs::is_directory(path)) {
        return PreviewType::DIRECTORY;
    }
    
    if (fs::is_regular_file(path)) {
        if (fs::file_size(path) == 0) {
            return PreviewType::EMPTY;
        }
        
        // Check file extension for known types
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        
        // Image files
        if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".gif" || 
            ext == ".bmp" || ext == ".svg" || ext == ".webp") {
            return PreviewType::IMAGE;
        }
        
        // Archive files
        if (ext == ".zip" || ext == ".tar" || ext == ".gz" || ext == ".bz2" || 
            ext == ".xz" || ext == ".7z" || ext == ".rar") {
            return PreviewType::ARCHIVE;
        }
        
        // Check if it's a text file
        if (is_text_file(path)) {
            return PreviewType::TEXT;
        }
        
        return PreviewType::BINARY;
    }
    
    return PreviewType::ERROR;
}

// Preview text file
PreviewContent QuickView::preview_text_file(const fs::path& path) {
    PreviewContent content;
    content.type = PreviewType::TEXT;
    content.file_size = fs::file_size(path);
    
    std::ifstream file(path);
    if (!file) {
        content.type = PreviewType::ERROR;
        content.error_message = "Cannot open file";
        return content;
    }
    
    std::string line;
    size_t line_count = 0;
    
    while (std::getline(file, line) && line_count < MAX_PREVIEW_LINES) {
        content.lines.push_back(truncate_line(line, MAX_LINE_LENGTH));
        line_count++;
    }
    
    // Count total lines if file has more
    while (std::getline(file, line)) {
        line_count++;
    }
    
    content.total_lines = line_count;
    return content;
}

// Preview directory
PreviewContent QuickView::preview_directory(const fs::path& path) {
    PreviewContent content;
    content.type = PreviewType::DIRECTORY;
    
    try {
        std::vector<fs::directory_entry> entries;
        for (const auto& entry : fs::directory_iterator(path)) {
            entries.push_back(entry);
        }
        
        // Sort entries: directories first, then by name
        std::sort(entries.begin(), entries.end(), 
            [](const auto& a, const auto& b) {
                bool a_is_dir = a.is_directory();
                bool b_is_dir = b.is_directory();
                if (a_is_dir != b_is_dir) {
                    return a_is_dir;
                }
                return a.path().filename() < b.path().filename();
            });
        
        content.lines.push_back("Directory: " + path.string());
        content.lines.push_back("Entries: " + std::to_string(entries.size()));
        content.lines.push_back("");
        
        size_t count = 0;
        for (const auto& entry : entries) {
            if (count >= MAX_PREVIEW_LINES - 3) {
                content.lines.push_back("... and " + 
                    std::to_string(entries.size() - count) + " more entries");
                break;
            }
            
            std::string line;
            if (entry.is_directory()) {
                line = "[DIR]  ";
            } else if (entry.is_symlink()) {
                line = "[LINK] ";
            } else {
                line = "[FILE] ";
            }
            
            line += entry.path().filename().string();
            
            if (entry.is_regular_file()) {
                line += " (" + format_size(entry.file_size()) + ")";
            }
            
            content.lines.push_back(line);
            count++;
        }
        
        content.total_lines = entries.size() + 3;
    } catch (const std::exception& e) {
        content.type = PreviewType::ERROR;
        content.error_message = "Cannot read directory: " + std::string(e.what());
    }
    
    return content;
}

// Preview binary file
PreviewContent QuickView::preview_binary_file(const fs::path& path) {
    PreviewContent content;
    content.type = PreviewType::BINARY;
    content.file_size = fs::file_size(path);
    
    content.lines.push_back("Binary file");
    content.lines.push_back("Size: " + format_size(content.file_size));
    content.lines.push_back("Permissions: " + format_permissions(path));
    
    // Show hex dump of first few bytes
    std::ifstream file(path, std::ios::binary);
    if (file) {
        content.lines.push_back("");
        content.lines.push_back("Hex dump (first 256 bytes):");
        content.lines.push_back("");
        
        char buffer[256];
        file.read(buffer, sizeof(buffer));
        size_t read_size = file.gcount();
        
        for (size_t i = 0; i < read_size; i += 16) {
            std::ostringstream hex_line;
            hex_line << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << i << "  ";
            
            // Hex values
            for (size_t j = 0; j < 16 && i + j < read_size; j++) {
                hex_line << std::setw(2) << (static_cast<unsigned>(buffer[i + j]) & 0xFF) << " ";
                if (j == 7) hex_line << " ";
            }
            
            // Padding
            for (size_t j = read_size - i; j < 16; j++) {
                hex_line << "   ";
                if (j == 7) hex_line << " ";
            }
            
            hex_line << " |";
            
            // ASCII representation
            for (size_t j = 0; j < 16 && i + j < read_size; j++) {
                char c = buffer[i + j];
                hex_line << (std::isprint(c) ? c : '.');
            }
            hex_line << "|";
            
            content.lines.push_back(hex_line.str());
        }
    }
    
    return content;
}

// Preview image file (metadata only)
PreviewContent QuickView::preview_image_file(const fs::path& path) {
    PreviewContent content;
    content.type = PreviewType::IMAGE;
    content.file_size = fs::file_size(path);
    
    content.lines.push_back("Image file: " + path.filename().string());
    content.lines.push_back("Size: " + format_size(content.file_size));
    content.lines.push_back("Type: " + path.extension().string());
    content.lines.push_back("");
    content.lines.push_back("[Image preview not available in terminal]");
    content.lines.push_back("");
    content.lines.push_back("Use 'O' to open with system viewer");
    
    return content;
}

// Preview archive file
PreviewContent QuickView::preview_archive_file(const fs::path& path) {
    PreviewContent content;
    content.type = PreviewType::ARCHIVE;
    content.file_size = fs::file_size(path);
    
    content.lines.push_back("Archive file: " + path.filename().string());
    content.lines.push_back("Size: " + format_size(content.file_size));
    content.lines.push_back("Type: " + path.extension().string());
    content.lines.push_back("");
    content.lines.push_back("[Archive contents preview not available]");
    content.lines.push_back("");
    content.lines.push_back("Use system tools to explore archive contents");
    
    return content;
}

// Main preview generation function
PreviewContent QuickView::generate_preview(const fs::path& path) {
    PreviewType type = detect_file_type(path);
    
    switch (type) {
        case PreviewType::TEXT:
            return preview_text_file(path);
        case PreviewType::DIRECTORY:
            return preview_directory(path);
        case PreviewType::IMAGE:
            return preview_image_file(path);
        case PreviewType::ARCHIVE:
            return preview_archive_file(path);
        case PreviewType::BINARY:
            return preview_binary_file(path);
        case PreviewType::EMPTY:
            {
                PreviewContent content;
                content.type = PreviewType::EMPTY;
                content.lines.push_back("Empty file");
                content.file_size = 0;
                return content;
            }
        case PreviewType::ERROR:
        default:
            {
                PreviewContent content;
                content.type = PreviewType::ERROR;
                content.error_message = "Cannot preview file";
                content.lines.push_back("Error: Cannot preview this file");
                return content;
            }
    }
}

// Format preview for display
std::vector<std::string> QuickView::format_preview(const PreviewContent& content, 
                                                   size_t width, size_t height) {
    std::vector<std::string> formatted;
    
    // Reserve space for header and footer
    size_t available_lines = height - 2;
    
    for (size_t i = 0; i < content.lines.size() && i < available_lines; i++) {
        formatted.push_back(truncate_line(content.lines[i], width - 2));
    }
    
    // Add indicator if there are more lines
    if (content.total_lines > available_lines) {
        formatted.push_back("... (" + std::to_string(content.total_lines - available_lines) + 
                          " more lines)");
    }
    
    return formatted;
}

// TabManager implementation
void TabManager::switch_to_tab(int tab_number) {
    if (tab_number == 1) {
        current_tab = MarkPaneTab::QUICKVIEW;
    } else if (tab_number == 2) {
        current_tab = MarkPaneTab::MARKED_FILES;
    }
}

void TabManager::activate_quickview(const fs::path& path) {
    quickview_active = true;
    current_preview_path = path;
    update_preview(path);
}

void TabManager::deactivate_quickview() {
    quickview_active = false;
    current_preview_path.clear();
    cached_preview = PreviewContent();
}

void TabManager::update_preview(const fs::path& path) {
    current_preview_path = path;
    cached_preview = QuickView::generate_preview(path);
}