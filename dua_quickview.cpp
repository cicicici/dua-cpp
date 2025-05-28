// dua_quickview.cpp - Quick file preview implementation
#include "dua_quickview.h"
#include <iomanip>
#include <cstring>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <sstream>

// ScrollableView implementation
void ScrollableView::move_up() {
    if (cursor_y > 0) {
        cursor_y--;
        
        // Adjust cursor_x if new line is shorter
        if (cursor_y < line_lengths.size()) {
            size_t line_len = line_lengths[cursor_y];
            if (line_len == 0) {
                cursor_x = 0;  // Empty line
            } else if (cursor_x >= line_len) {
                cursor_x = line_len - 1;  // Stay at last character
            }
        }
        
        // Adjust view if cursor moves above visible area
        if (cursor_y < view_offset_y) {
            view_offset_y = cursor_y;
        }
    }
}

void ScrollableView::move_down() {
    if (cursor_y < content_height - 1) {
        cursor_y++;
        
        // Adjust cursor_x if new line is shorter
        if (cursor_y < line_lengths.size()) {
            size_t line_len = line_lengths[cursor_y];
            if (line_len == 0) {
                cursor_x = 0;  // Empty line
            } else if (cursor_x >= line_len) {
                cursor_x = line_len - 1;  // Stay at last character
            }
        }
        
        // Adjust view if cursor moves below visible area
        if (cursor_y >= view_offset_y + window_height) {
            view_offset_y = cursor_y - window_height + 1;
        }
    }
}

void ScrollableView::move_left() {
    if (cursor_x > 0) {
        cursor_x--;
        // Adjust view if cursor moves left of visible area
        if (cursor_x < view_offset_x) {
            view_offset_x = cursor_x;
        }
    }
}

void ScrollableView::move_right() {
    // Limit cursor to last character (not past it)
    size_t current_line_length = 0;
    if (cursor_y < line_lengths.size()) {
        current_line_length = line_lengths[cursor_y];
    }
    
    // For non-empty lines, stop at last character
    if (current_line_length > 0 && cursor_x < current_line_length - 1) {
        cursor_x++;
        // Adjust view if cursor moves right of visible area
        if (cursor_x >= view_offset_x + window_width) {
            view_offset_x = cursor_x - window_width + 1;
        }
    }
    // For empty lines, cursor stays at position 0
}

void ScrollableView::page_up() {
    if (cursor_y >= window_height) {
        cursor_y -= window_height;
        view_offset_y = (view_offset_y >= window_height) ? view_offset_y - window_height : 0;
    } else {
        cursor_y = 0;
        view_offset_y = 0;
    }
}

void ScrollableView::page_down() {
    cursor_y = std::min(cursor_y + window_height, content_height - 1);
    
    // Adjust view to show cursor
    if (cursor_y >= view_offset_y + window_height) {
        view_offset_y = std::min(cursor_y - window_height + 1, 
                                 content_height > window_height ? content_height - window_height : 0);
    }
}

void ScrollableView::move_home() {
    cursor_y = 0;
    view_offset_y = 0;
}

void ScrollableView::move_end() {
    cursor_y = content_height > 0 ? content_height - 1 : 0;
    view_offset_y = content_height > window_height ? content_height - window_height : 0;
}

void ScrollableView::move_line_start() {
    cursor_x = 0;
    view_offset_x = 0;
}

void ScrollableView::move_line_end() {
    // Move to last character (not after it)
    if (cursor_y < line_lengths.size() && line_lengths[cursor_y] > 0) {
        cursor_x = line_lengths[cursor_y] - 1;  // Position at last character
    } else {
        cursor_x = 0;  // Empty line stays at 0
    }
    
    // Adjust view to show cursor
    if (cursor_x >= view_offset_x + window_width) {
        view_offset_x = cursor_x > window_width ? cursor_x - window_width + 1 : 0;
    }
}

void ScrollableView::update_window_size(size_t width, size_t height) {
    window_width = width;
    window_height = height;
    
    // Adjust view offsets if necessary
    if (view_offset_y + window_height > content_height && content_height > 0) {
        view_offset_y = content_height > window_height ? content_height - window_height : 0;
    }
    if (view_offset_x + window_width > content_width && content_width > 0) {
        view_offset_x = content_width > window_width ? content_width - window_width : 0;
    }
}

void ScrollableView::update_content_info(const std::vector<std::string>& lines) {
    content_height = lines.size();
    max_line_length = 0;
    line_lengths.clear();
    line_lengths.reserve(lines.size());
    
    for (const auto& line : lines) {
        line_lengths.push_back(line.length());
        max_line_length = std::max(max_line_length, line.length());
    }
    
    content_width = max_line_length;  // Limit to actual content width
    
    // Reset cursor and view if out of bounds
    if (cursor_y >= content_height) {
        cursor_y = content_height > 0 ? content_height - 1 : 0;
    }
    // Don't limit cursor_x anymore - allow free movement
    
    // Adjust view offsets
    if (view_offset_y + window_height > content_height) {
        view_offset_y = content_height > window_height ? content_height - window_height : 0;
    }
    if (view_offset_x + window_width > content_width) {
        view_offset_x = content_width > window_width ? content_width - window_width : 0;
    }
}

void ScrollableView::reset() {
    cursor_x = 0;
    cursor_y = 0;
    view_offset_x = 0;
    view_offset_y = 0;
    max_line_length = 0;
    line_lengths.clear();
    search_matches.clear();
    search_pattern.clear();
    current_match_index = 0;
    search_active = false;
    command_active = false;
    command_buffer.clear();
}

// Search implementation
void ScrollableView::start_search() {
    search_active = true;
    search_pattern.clear();
    search_matches.clear();
    current_match_index = 0;
}

void ScrollableView::end_search() {
    search_active = false;
    // Keep matches and pattern for navigation with n/N
}

void ScrollableView::perform_search(const std::vector<std::string>& lines) {
    search_matches.clear();
    current_match_index = 0;
    
    if (search_pattern.empty()) {
        return;
    }
    
    // Convert search pattern to lowercase for case-insensitive search
    std::string lower_pattern = search_pattern;
    std::transform(lower_pattern.begin(), lower_pattern.end(), lower_pattern.begin(), ::tolower);
    
    // Search through all lines
    for (size_t line_idx = 0; line_idx < lines.size(); line_idx++) {
        const std::string& line = lines[line_idx];
        std::string lower_line = line;
        std::transform(lower_line.begin(), lower_line.end(), lower_line.begin(), ::tolower);
        
        // Find all occurrences in this line
        size_t pos = 0;
        while ((pos = lower_line.find(lower_pattern, pos)) != std::string::npos) {
            search_matches.push_back({line_idx, pos});
            pos += lower_pattern.length();
        }
    }
    
    // If we have matches, move to the first one
    if (!search_matches.empty()) {
        // Find the closest match to current cursor position
        size_t best_match = 0;
        size_t min_distance = SIZE_MAX;
        
        for (size_t i = 0; i < search_matches.size(); i++) {
            size_t line_dist = (search_matches[i].line > cursor_y) ? 
                              search_matches[i].line - cursor_y : 
                              cursor_y - search_matches[i].line;
            size_t col_dist = (search_matches[i].column > cursor_x) ? 
                             search_matches[i].column - cursor_x : 
                             cursor_x - search_matches[i].column;
            size_t distance = line_dist * 1000 + col_dist;  // Prioritize line distance
            
            if (distance < min_distance) {
                min_distance = distance;
                best_match = i;
            }
        }
        
        current_match_index = best_match;
        move_to_match(current_match_index);
    }
}

void ScrollableView::search_word_under_cursor(const std::vector<std::string>& lines) {
    // Get the current line
    if (cursor_y >= lines.size()) return;
    
    const std::string& line = lines[cursor_y];
    if (cursor_x >= line.length()) return;
    
    // Find word boundaries
    size_t word_start = cursor_x;
    size_t word_end = cursor_x;
    
    // Define what constitutes a word character (alphanumeric and underscore)
    auto is_word_char = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    };
    
    // If not on a word character, do nothing
    if (!is_word_char(line[cursor_x])) {
        return;
    }
    
    // Find start of word
    while (word_start > 0 && is_word_char(line[word_start - 1])) {
        word_start--;
    }
    
    // Find end of word
    while (word_end < line.length() && is_word_char(line[word_end])) {
        word_end++;
    }
    
    // Extract the word
    if (word_start < word_end) {
        search_pattern = line.substr(word_start, word_end - word_start);
        search_active = false;  // Not in search input mode
        perform_search(lines);
        
        // Move to next match after current position
        next_match();
    }
}

void ScrollableView::next_match() {
    if (search_matches.empty()) return;
    
    // Find the first match after current cursor position
    size_t best_match = 0;
    bool found_after = false;
    
    for (size_t i = 0; i < search_matches.size(); i++) {
        const SearchMatch& match = search_matches[i];
        
        // Check if this match is after current cursor position
        if (match.line > cursor_y || (match.line == cursor_y && match.column > cursor_x)) {
            best_match = i;
            found_after = true;
            break;
        }
    }
    
    // If no match after cursor, wrap to first match
    if (!found_after && !search_matches.empty()) {
        best_match = 0;
    }
    
    current_match_index = best_match;
    move_to_match(current_match_index);
}

void ScrollableView::prev_match() {
    if (search_matches.empty()) return;
    
    // Find the last match before current cursor position
    size_t best_match = search_matches.size() - 1;  // Default to last match (for wrapping)
    
    // Search backwards through matches
    for (int i = search_matches.size() - 1; i >= 0; i--) {
        const SearchMatch& match = search_matches[i];
        
        // Check if this match is before current cursor position
        if (match.line < cursor_y || (match.line == cursor_y && match.column < cursor_x)) {
            best_match = i;
            break;
        }
    }
    
    current_match_index = best_match;
    move_to_match(current_match_index);
}

void ScrollableView::move_to_match(size_t match_index) {
    if (match_index >= search_matches.size()) return;
    
    const SearchMatch& match = search_matches[match_index];
    cursor_y = match.line;
    cursor_x = match.column;
    
    // Update current match index for display
    current_match_index = match_index;
    
    // Center the match in the view if possible
    if (window_height > 0) {
        size_t target_offset_y = (match.line > window_height / 2) ? 
                                match.line - window_height / 2 : 0;
        if (target_offset_y + window_height > content_height) {
            view_offset_y = content_height > window_height ? content_height - window_height : 0;
        } else {
            view_offset_y = target_offset_y;
        }
    }
    
    // Ensure horizontal visibility
    if (cursor_x < view_offset_x) {
        view_offset_x = cursor_x;
    } else if (cursor_x >= view_offset_x + window_width) {
        view_offset_x = cursor_x > window_width ? cursor_x - window_width / 2 : 0;
    }
}

void ScrollableView::update_current_match_index() {
    if (search_matches.empty()) {
        current_match_index = 0;
        return;
    }
    
    // Find which match the cursor is at or closest to
    for (size_t i = 0; i < search_matches.size(); i++) {
        const SearchMatch& match = search_matches[i];
        if (match.line == cursor_y && match.column == cursor_x) {
            current_match_index = i;
            return;
        }
    }
    
    // If not on a match, find the next match position for reference
    for (size_t i = 0; i < search_matches.size(); i++) {
        const SearchMatch& match = search_matches[i];
        if (match.line > cursor_y || (match.line == cursor_y && match.column > cursor_x)) {
            // We're before this match, so the "current" is the previous one
            current_match_index = (i > 0) ? i - 1 : search_matches.size() - 1;
            return;
        }
    }
    
    // Cursor is after all matches
    current_match_index = search_matches.size() - 1;
}

// Command mode implementation
void ScrollableView::start_command() {
    command_active = true;
    command_buffer.clear();
}

void ScrollableView::end_command() {
    command_active = false;
    command_buffer.clear();
}

void ScrollableView::execute_command() {
    if (command_buffer.empty()) {
        end_command();
        return;
    }
    
    // Check for special commands
    if (command_buffer == "$") {
        // Go to last line
        if (content_height > 0) {
            goto_line(content_height);
        }
    } else {
        // Try to parse as line number
        try {
            size_t line_num = std::stoull(command_buffer);
            if (line_num > 0) {
                goto_line(line_num);
            }
        } catch (...) {
            // Invalid command, just ignore
        }
    }
    
    end_command();
}

void ScrollableView::goto_line(size_t line_number) {
    // Convert from 1-based to 0-based indexing
    if (line_number > 0) {
        line_number--;
    }
    
    // Clamp to valid range
    if (line_number >= content_height) {
        line_number = content_height > 0 ? content_height - 1 : 0;
    }
    
    cursor_y = line_number;
    
    // Reset cursor x to beginning of line
    cursor_x = 0;
    
    // Center the line in view if possible
    if (window_height > 0) {
        if (line_number > window_height / 2) {
            view_offset_y = line_number - window_height / 2;
            if (view_offset_y + window_height > content_height) {
                view_offset_y = content_height > window_height ? content_height - window_height : 0;
            }
        } else {
            view_offset_y = 0;
        }
    }
    
    // Reset horizontal scroll
    view_offset_x = 0;
    
    // Update match index for display
    if (!search_matches.empty()) {
        update_current_match_index();
    }
}

// Check if syntax highlighter (bat) is available
bool QuickView::has_syntax_highlighter() {
    static int has_bat = -1;  // Cache the result
    if (has_bat == -1) {
        has_bat = (system("which bat > /dev/null 2>&1") == 0) ? 1 : 0;
    }
    return has_bat == 1;
}

// Map RGB colors to closest ncurses color
int rgb_to_curses_color(int r, int g, int b) {
    // More sophisticated color mapping for syntax highlighting
    
    // Special cases for common syntax highlighting colors from Monokai theme
    
    // Keywords (bright magenta/pink) - RGB(249, 38, 114)
    if (r > 240 && r < 255 && g < 50 && b > 100 && b < 120) return 21;
    
    // Strings (yellow) - RGB(230, 219, 116)
    if (r > 220 && r < 240 && g > 210 && g < 230 && b > 110 && b < 130) return 19;
    
    // Comments (gray) - RGB(117, 113, 94)
    if (r > 110 && r < 125 && g > 105 && g < 120 && b > 85 && b < 100) return 24;
    
    // Types/Classes (cyan) - RGB(102, 217, 239)
    if (r > 95 && r < 110 && g > 210 && g < 225 && b > 230) return 22;
    
    // Functions (green) - RGB(166, 226, 46)
    if (r > 160 && r < 175 && g > 220 && g < 235 && b < 60) return 18;
    
    // Variables (orange) - RGB(253, 151, 31)
    if (r > 245 && g > 145 && g < 160 && b < 40) return 27;
    
    // Regular text (off-white) - RGB(248, 248, 242)
    if (r > 240 && g > 240 && b > 235) return 23;
    
    // Calculate color characteristics for general mapping
    int max_val = std::max({r, g, b});
    int min_val = std::min({r, g, b});
    int range = max_val - min_val;
    int luminance = (r * 299 + g * 587 + b * 114) / 1000;
    
    // Pure black
    if (max_val < 30) return 16;
    
    // Gray scale (low saturation)
    if (range < 30) {
        if (luminance > 200) return 23;  // White
        if (luminance > 150) return 31;  // Bright white
        if (luminance > 100) return 24;  // Gray
        if (luminance > 50) return 16;   // Dark gray
        return 16;  // Black
    }
    
    // High saturation colors
    if (r > g && r > b) {
        // Red dominant
        if (r - g > 80 && r - b > 80) return 17;  // Pure red
        if (r > 200 && g > 150) return 19;  // Yellow/orange
        if (r > 150 && b > 100) return 21;  // Magenta
    } else if (g > r && g > b) {
        // Green dominant
        if (g - r > 80 && g - b > 80) return 18;  // Pure green
        if (g > 200 && b > 150) return 22;  // Cyan
        if (g > 150 && r > 100) return 19;  // Yellow-green
    } else if (b > r && b > g) {
        // Blue dominant
        if (b - r > 80 && b - g > 80) return 20;  // Pure blue
        if (b > 200 && g > 150) return 22;  // Cyan
        if (b > 150 && r > 100) return 21;  // Magenta/purple
    }
    
    // Default based on luminance
    return luminance > 150 ? 23 : 24;
}

// Parse ANSI escape codes and create styled lines
std::vector<StyledLine> parse_ansi_text(const std::string& ansi_text) {
    std::vector<StyledLine> result;
    std::istringstream stream(ansi_text);
    std::string line;
    
    while (std::getline(stream, line)) {
        StyledLine styled_line;
        styled_line.plain_text = "";
        
        size_t pos = 0;
        int current_color = 0;
        int current_attrs = 0;
        
        while (pos < line.length()) {
            if (line[pos] == '\033' && pos + 1 < line.length() && line[pos + 1] == '[') {
                // Parse ANSI escape sequence
                pos += 2;
                std::string code;
                while (pos < line.length() && line[pos] != 'm') {
                    code += line[pos++];
                }
                pos++; // Skip 'm'
                
                // Parse SGR parameters
                std::vector<int> params;
                std::istringstream code_stream(code);
                std::string param;
                while (std::getline(code_stream, param, ';')) {
                    try {
                        params.push_back(std::stoi(param));
                    } catch (...) {
                        // Ignore invalid parameters
                    }
                }
                
                // Process parameters
                for (size_t i = 0; i < params.size(); i++) {
                    int val = params[i];
                    if (val == 0) {
                        current_color = 0;
                        current_attrs = 0;
                    } else if (val == 1) {
                        current_attrs |= A_BOLD;
                    } else if (val == 4) {
                        current_attrs |= A_UNDERLINE;
                    } else if (val == 38 && i + 4 < params.size() && params[i + 1] == 2) {
                        // 24-bit RGB color: 38;2;R;G;B
                        int r = params[i + 2];
                        int g = params[i + 3];
                        int b = params[i + 4];
                        current_color = rgb_to_curses_color(r, g, b);
                        // Debug: Log the color mapping
                        // fprintf(stderr, "RGB(%d,%d,%d) -> Color %d\n", r, g, b, current_color);
                        i += 4; // Skip the RGB values
                    } else if (val >= 30 && val <= 37) {
                        // Basic 16-color ANSI
                        current_color = 16 + (val - 30);
                    } else if (val >= 90 && val <= 97) {
                        // Bright 16-color ANSI
                        current_color = 24 + (val - 90);
                    }
                }
            } else {
                // Regular character
                StyledChar sc;
                sc.ch = line[pos];
                sc.color_pair = current_color;
                sc.attrs = current_attrs;
                styled_line.styled_chars.push_back(sc);
                styled_line.plain_text += line[pos];
                pos++;
            }
        }
        
        result.push_back(styled_line);
    }
    
    return result;
}

// Strip ANSI codes for plain text
std::string QuickView::strip_ansi_codes(const std::string& text) {
    std::string result;
    bool in_escape = false;
    
    for (size_t i = 0; i < text.length(); i++) {
        if (text[i] == '\033' && i + 1 < text.length() && text[i + 1] == '[') {
            in_escape = true;
            i++; // Skip '['
        } else if (in_escape && text[i] == 'm') {
            in_escape = false;
        } else if (!in_escape) {
            result += text[i];
        }
    }
    
    return result;
}

// Preview text file with syntax highlighting
PreviewContent QuickView::preview_text_file_with_highlighting(const fs::path& path) {
    PreviewContent content;
    content.type = PreviewType::TEXT;
    
    try {
        // Use bat for syntax highlighting
        // Escape the file path for shell
        std::string escaped_path = "'" + path.string() + "'";
        // Use Monokai Extended theme for richer syntax highlighting
        std::string cmd = "bat --color=always --style=plain --theme='Monokai Extended' --paging=never --line-range=1:" + 
                         std::to_string(MAX_PREVIEW_LINES) + " " + escaped_path + " 2>/dev/null";
        
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            return preview_text_file(path);  // Fallback to plain text
        }
        
        std::string result;
        char buffer[1024];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            result += buffer;
        }
        pclose(pipe);
        
        if (result.empty()) {
            return preview_text_file(path);  // Fallback to plain text
        }
        
        // Parse ANSI codes
        content.styled_lines = parse_ansi_text(result);
        content.has_highlighting = true;
        
        // Also store plain text for search functionality
        for (const auto& styled_line : content.styled_lines) {
            content.lines.push_back(styled_line.plain_text);
        }
        
        content.total_lines = content.lines.size();
        content.file_size = fs::file_size(path);
        
    } catch (...) {
        return preview_text_file(path);  // Fallback to plain text
    }
    
    return content;
}

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
            // Try syntax highlighting first if available
            if (has_syntax_highlighter()) {
                return preview_text_file_with_highlighting(path);
            }
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
    scroll_view.reset();
    scroll_view.update_content_info(cached_preview.lines);
}

void TabManager::deactivate_quickview() {
    quickview_active = false;
    current_preview_path.clear();
    cached_preview = PreviewContent();
    scroll_view.reset();
}

void TabManager::update_preview(const fs::path& path) {
    current_preview_path = path;
    cached_preview = QuickView::generate_preview(path);
    scroll_view.update_content_info(cached_preview.lines);
}