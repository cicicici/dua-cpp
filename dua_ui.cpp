// dua_ui.cpp - UI functionality implementation
#include "dua_ui.h"
#include <ctime>
#include <cstring>

// LineCache implementation
bool LineCache::operator!=(const LineCache& other) const {
    return content != other.content || 
           attributes != other.attributes || 
           is_selected != other.is_selected;
}

// MarkPane implementation
MarkPane::MarkPane(Config& cfg) : config(cfg) {}

void MarkPane::set_focus(bool focus) {
    has_focus = focus;
    if (focus && !marked_items.empty()) {
        selected_index = marked_items.size() - 1;
        adjust_view_offset();
    }
}

bool MarkPane::is_focused() const { return has_focus; }
bool MarkPane::is_empty() const { return marked_items.empty(); }
size_t MarkPane::count() const { return marked_items.size(); }

uintmax_t MarkPane::total_size() const {
    return std::accumulate(marked_sizes.begin(), marked_sizes.end(), uintmax_t(0));
}

void MarkPane::update_marked_items(const std::vector<std::shared_ptr<Entry>>& roots) {
    marked_items.clear();
    marked_paths.clear();
    marked_sizes.clear();
    
    for (const auto& root : roots) {
        collect_marked_recursive(root);
    }
    
    // Sort by path
    std::vector<size_t> indices(marked_items.size());
    std::iota(indices.begin(), indices.end(), 0);
    std::sort(indices.begin(), indices.end(), 
        [this](size_t a, size_t b) {
            return marked_paths[a] < marked_paths[b];
        });
    
    // Reorder vectors
    std::vector<std::shared_ptr<Entry>> sorted_items;
    std::vector<std::string> sorted_paths;
    std::vector<uintmax_t> sorted_sizes;
    
    for (size_t i : indices) {
        sorted_items.push_back(marked_items[i]);
        sorted_paths.push_back(marked_paths[i]);
        sorted_sizes.push_back(marked_sizes[i]);
    }
    
    marked_items = std::move(sorted_items);
    marked_paths = std::move(sorted_paths);
    marked_sizes = std::move(sorted_sizes);
    
    if (selected_index >= marked_items.size() && !marked_items.empty()) {
        selected_index = marked_items.size() - 1;
    }
    adjust_view_offset();
}

void MarkPane::navigate_up() {
    if (selected_index > 0) {
        selected_index--;
        adjust_view_offset();
    }
}

void MarkPane::navigate_down() {
    if (selected_index + 1 < marked_items.size()) {
        selected_index++;
        adjust_view_offset();
    }
}

void MarkPane::navigate_page_up() {
    if (selected_index > 10) {
        selected_index -= 10;
    } else {
        selected_index = 0;
    }
    adjust_view_offset();
}

void MarkPane::navigate_page_down() {
    selected_index = std::min(selected_index + 10, marked_items.size() - 1);
    adjust_view_offset();
}

void MarkPane::navigate_home() {
    selected_index = 0;
    view_offset = 0;
}

void MarkPane::navigate_end() {
    if (!marked_items.empty()) {
        selected_index = marked_items.size() - 1;
        adjust_view_offset();
    }
}

void MarkPane::remove_selected() {
    if (selected_index < marked_items.size()) {
        marked_items[selected_index]->marked = false;
        marked_items.erase(marked_items.begin() + selected_index);
        marked_paths.erase(marked_paths.begin() + selected_index);
        marked_sizes.erase(marked_sizes.begin() + selected_index);
        
        if (selected_index >= marked_items.size() && !marked_items.empty()) {
            selected_index = marked_items.size() - 1;
        }
        adjust_view_offset();
    }
}

void MarkPane::remove_all() {
    for (auto& item : marked_items) {
        item->marked = false;
    }
    marked_items.clear();
    marked_paths.clear();
    marked_sizes.clear();
    selected_index = 0;
    view_offset = 0;
}

std::vector<std::shared_ptr<Entry>> MarkPane::get_all_marked() const {
    return marked_items;
}

void MarkPane::draw(WINDOW* win, int height, int width) {
    werase(win);
    box(win, 0, 0);
    
    // Title
    std::string title = " Marked Items (" + std::to_string(marked_items.size()) + 
                       " items, " + format_size(total_size(), config.format) + ") ";
    mvwprintw(win, 0, (width - title.length()) / 2, "%s", title.c_str());
    
    // Help text
    if (has_focus) {
        wattron(win, A_BOLD);
        mvwprintw(win, 0, width - 30, " x/d/space = remove | a = all ");
        wattroff(win, A_BOLD);
    }
    
    int visible_height = height - 2;
    int content_width = width - 2;
    
    // Draw items
    for (int i = 0; i < visible_height && view_offset + i < marked_items.size(); i++) {
        size_t item_idx = view_offset + i;
        bool is_selected = has_focus && (item_idx == selected_index);
        
        wmove(win, i + 1, 1);
        
        if (is_selected) {
            wattron(win, A_REVERSE);
            for (int j = 0; j < content_width; j++) {
                waddch(win, ' ');
            }
            wmove(win, i + 1, 1);
        }
        
        std::string size_str = format_size(marked_sizes[item_idx], config.format);
        int size_width = 12;
        int separator_width = 3;
        int path_width = content_width - size_width - separator_width;
        
        std::string path = marked_paths[item_idx];
        if (path.length() > static_cast<size_t>(path_width)) {
            path = "..." + path.substr(path.length() - path_width + 3);
        }
        
        wattron(win, COLOR_PAIR(3));
        mvwprintw(win, i + 1, 1, "%*s", size_width, size_str.c_str());
        wattroff(win, COLOR_PAIR(3));
        
        wprintw(win, " | ");
        
        auto& item = marked_items[item_idx];
        if (item->is_symlink) {
            wattron(win, COLOR_PAIR(9));
        } else if (item->is_directory) {
            wattron(win, COLOR_PAIR(1) | A_BOLD);
        }
        
        wprintw(win, "%s", path.c_str());
        
        if (item->is_symlink || item->is_directory) {
            wattroff(win, COLOR_PAIR(item->is_symlink ? 9 : 1) | 
                    (item->is_directory ? A_BOLD : 0));
        }
        
        if (is_selected) {
            wattroff(win, A_REVERSE);
        }
    }
    
    if (marked_items.size() > static_cast<size_t>(visible_height)) {
        draw_scrollbar(win, height, view_offset, marked_items.size(), visible_height);
    }
    
    if (has_focus) {
        mvwhline(win, height - 1, 1, ACS_HLINE, width - 2);
        mvwprintw(win, height - 1, 2, " Ctrl+r = delete | Ctrl+t = trash ");
    }
    
    wrefresh(win);
}

void MarkPane::collect_marked_recursive(std::shared_ptr<Entry> entry) {
    if (entry->marked.load()) {
        marked_items.push_back(entry);
        marked_paths.push_back(entry->path.string());
        marked_sizes.push_back(entry->size.load());
    }
    
    if (entry->is_directory && !entry->is_symlink) {
        std::lock_guard<std::mutex> lock(entry->children_mutex);
        for (auto& child : entry->children) {
            collect_marked_recursive(child);
        }
    }
}

void MarkPane::adjust_view_offset() {
    int visible_height = 20;
    
    if (static_cast<int>(selected_index) < static_cast<int>(view_offset)) {
        view_offset = selected_index;
    } else if (static_cast<int>(selected_index) >= static_cast<int>(view_offset) + visible_height) {
        view_offset = selected_index - visible_height + 1;
    }
}

void MarkPane::draw_scrollbar(WINDOW* win, int height, size_t offset, size_t total, int visible) {
    int bar_height = height - 2;
    int bar_pos = (offset * bar_height) / total;
    int bar_size = std::max(1, (visible * bar_height) / static_cast<int>(total));
    
    for (int i = 0; i < bar_height; i++) {
        mvwaddch(win, i + 1, getmaxx(win) - 1, 
                 (i >= bar_pos && i < bar_pos + bar_size) ? ACS_CKBOARD : ACS_VLINE);
    }
}

// InteractiveUI implementation
InteractiveUI::InteractiveUI(std::vector<std::shared_ptr<Entry>> root_entries, Config& cfg) 
    : roots(root_entries), config(cfg), mark_pane(cfg) {
    
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
    line_cache.reserve(LINES);
}

InteractiveUI::~InteractiveUI() {
    if (main_win) delwin(main_win);
    if (mark_win) delwin(mark_win);
}

void InteractiveUI::run() {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    nodelay(stdscr, TRUE);
    scrollok(stdscr, FALSE);
    idlok(stdscr, TRUE);
    idcok(stdscr, TRUE);
    
    if (has_colors()) {
        start_color();
        init_pair(1, COLOR_CYAN, COLOR_BLACK);
        init_pair(2, COLOR_WHITE, COLOR_BLACK);
        init_pair(3, COLOR_GREEN, COLOR_BLACK);
        init_pair(4, COLOR_BLACK, COLOR_CYAN);
        init_pair(5, COLOR_WHITE, COLOR_BLACK);
        init_pair(6, COLOR_YELLOW, COLOR_BLACK);
        init_pair(7, COLOR_BLUE, COLOR_BLACK);
        init_pair(8, COLOR_RED, COLOR_BLACK);
        init_pair(9, COLOR_MAGENTA, COLOR_BLACK);
    }
    
    update_window_layout();
    
    bool running = true;
    int pending_move = 0;
    
    while (running) {
        if (!mark_pane.is_empty() || has_any_marked_items()) {
            mark_pane.update_marked_items(roots);
        }
        
        if (needs_full_redraw) {
            draw_full();
            needs_full_redraw = false;
        } else {
            draw_differential();
        }
        
        if (!mark_pane.is_empty()) {
            mark_pane.draw(mark_win, getmaxy(mark_win), getmaxx(mark_win));
        }
        
        int ch = getch();
        if (ch != ERR) {
            // Handle terminal resize
            if (ch == KEY_RESIZE) {
                handle_resize();
                continue;
            }
            
            auto now = std::chrono::steady_clock::now();
            bool is_movement = (ch == KEY_UP || ch == KEY_DOWN || ch == 'j' || ch == 'k');
            
            if (ch == '\t' && !mark_pane.is_empty()) {
                switch_focus();
                needs_full_redraw = true;
                continue;
            }
            
            if (focused_pane == FocusedPane::Mark && mark_pane.is_focused()) {
                if (handle_mark_pane_key(ch) == false) {
                    running = false;
                }
            } else if (is_movement) {
                if (now - last_input_time < INPUT_BATCH_DELAY) {
                    pending_move += (ch == KEY_DOWN || ch == 'j') ? 1 : -1;
                    napms(1);
                    continue;
                }
                
                if (pending_move != 0) {
                    apply_movement(pending_move);
                    pending_move = 0;
                }
                
                if (ch == KEY_UP || ch == 'k') {
                    navigate_up();
                } else {
                    navigate_down();
                }
                
                last_input_time = now;
            } else {
                if (glob_search_active) {
                    handle_glob_search(ch);
                } else {
                    if (handle_key(ch) == false) {
                        running = false;
                    }
                }
            }
        } else {
            if (pending_move != 0) {
                apply_movement(pending_move);
                pending_move = 0;
            }
            napms(10);
        }
    }
    
    endwin();
    print_marked_paths();
}

// The rest of the InteractiveUI methods
void InteractiveUI::update_window_layout() {
    if (main_win) {
        delwin(main_win);
        main_win = nullptr;
    }
    if (mark_win) {
        delwin(mark_win);
        mark_win = nullptr;
    }
    
    clear();
    refresh();
    
    if (!mark_pane.is_empty()) {
        int width = COLS;
        int height = LINES;
        int split_pos = width * 2 / 3;
        
        main_win = newwin(height, split_pos, 0, 0);
        keypad(main_win, TRUE);
        nodelay(main_win, TRUE);
        
        mark_win = newwin(height, width - split_pos, 0, split_pos);
        keypad(mark_win, TRUE);
        nodelay(mark_win, TRUE);
    } else {
        main_win = newwin(LINES, COLS, 0, 0);
        keypad(main_win, TRUE);
        nodelay(main_win, TRUE);
    }
}

void InteractiveUI::switch_focus() {
    if (focused_pane == FocusedPane::Main) {
        focused_pane = FocusedPane::Mark;
        mark_pane.set_focus(true);
    } else {
        focused_pane = FocusedPane::Main;
        mark_pane.set_focus(false);
    }
}

// Navigation method implementations
void InteractiveUI::navigate_up() {
    if (selected_index > 0) {
        selected_index--;
        if (selected_index < view_offset) {
            view_offset = selected_index;
        }
    }
}

void InteractiveUI::navigate_down() {
    if (selected_index + 1 < current_view.size()) {
        selected_index++;
        int max_visible = LINES - 4;
        if (static_cast<int>(selected_index) >= static_cast<int>(view_offset) + max_visible) {
            view_offset = selected_index - max_visible + 1;
        }
    }
}

void InteractiveUI::apply_movement(int delta) {
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
        
        int max_visible = LINES - 4;
        if (static_cast<int>(selected_index) < static_cast<int>(view_offset)) {
            view_offset = selected_index;
        } else if (static_cast<int>(selected_index) >= static_cast<int>(view_offset) + max_visible) {
            view_offset = selected_index - max_visible + 1;
        }
    }
}

void InteractiveUI::update_view() {
    format_cache.clear();
    current_view.clear();
    {
        std::lock_guard<std::mutex> lock(current_dir->children_mutex);
        current_view = current_dir->children;
    }
    apply_sort();
}

void InteractiveUI::apply_sort() {
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

// Drawing method implementations
void InteractiveUI::draw_full() {
    WINDOW* win = main_win ? main_win : stdscr;
    int height = getmaxy(win);
    int width = getmaxx(win);
    
    werase(win);
    
    // Header
    wattron(win, A_REVERSE);
    mvwhline(win, 0, 0, ' ', width);
    mvwprintw(win, 0, 1, " Disk Usage Analyzer v1.2.0 [C++ Optimized]    (press ? for help)");
    wattroff(win, A_REVERSE);
    
    // Path bar
    wattron(win, A_REVERSE);
    mvwhline(win, 1, 0, ' ', width);
    std::string path_str = current_dir->path.string();
    if (path_str.empty()) path_str = "[root]";
    mvwprintw(win, 1, 1, " %s", path_str.c_str());
    
    // Stats on the right
    if (!current_view.empty()) {
        std::string info = "(" + std::to_string(current_view.size()) + " visible, " +
                          std::to_string(current_dir->entry_count.load()) + " total, " +
                          format_size(current_dir->size, config.format) + ")";
        if (info.length() + 2 < static_cast<size_t>(width)) {
            mvwprintw(win, 1, width - info.length() - 2, "%s", info.c_str());
        }
    }
    wattroff(win, A_REVERSE);
    
    // File list
    int y = 2;
    int max_y = height - 2;
    
    line_cache.clear();
    for (size_t i = view_offset; i < current_view.size() && y < max_y; i++) {
        draw_entry_line(i, y, true, win, width);
        y++;
    }
    
    // Status bar
    update_status_line(win, height, width);
    
    // Help line
    if (!glob_search_active && !show_help) {
        wmove(win, height - 1, 0);
        wclrtoeol(win);
        mvwprintw(win, height - 1, 1, " mark = d/space | ");
        if (!mark_pane.is_empty()) {
            wprintw(win, "mark pane = Tab | ");
        }
        wprintw(win, "delete = d | search = / | refresh = r/R");
    }
    
    if (show_help) {
        draw_help(win);
    }
    
    // Remember current state
    last_selected_index = selected_index;
    last_view_offset = view_offset;
    
    wrefresh(win);
}

void InteractiveUI::draw_differential() {
    WINDOW* win = main_win ? main_win : stdscr;
    int height = getmaxy(win);
    int width = getmaxx(win);
    
    bool selection_changed = (selected_index != last_selected_index);
    bool view_scrolled = (view_offset != last_view_offset);
    
    if (!selection_changed && !view_scrolled) {
        return;  // Nothing to update
    }
    
    int y = 2;
    int max_y = height - 2;
    
    // If view scrolled, we need to redraw all visible lines
    if (view_scrolled) {
        for (size_t i = view_offset; i < current_view.size() && y < max_y; i++) {
            draw_entry_line(i, y, true, win, width);
            y++;
        }
        
        // Clear any remaining lines
        while (y < max_y) {
            wmove(win, y, 0);
            wclrtoeol(win);
            y++;
        }
    } else if (selection_changed) {
        // Only update the old and new selected lines
        if (last_selected_index != SIZE_MAX && 
            last_selected_index >= view_offset && 
            last_selected_index < view_offset + (max_y - 2)) {
            int old_y = 2 + (last_selected_index - view_offset);
            draw_entry_line(last_selected_index, old_y, false, win, width);
        }
        
        if (selected_index >= view_offset && 
            selected_index < view_offset + (max_y - 2)) {
            int new_y = 2 + (selected_index - view_offset);
            draw_entry_line(selected_index, new_y, false, win, width);
        }
    }
    
    // Update status line if needed
    update_status_line(win, height, width);
    
    // Remember current state
    last_selected_index = selected_index;
    last_view_offset = view_offset;
    
    wrefresh(win);
}

void InteractiveUI::draw_entry_line(size_t index, int y, bool force_redraw, WINDOW* win, int win_width) {
    (void)force_redraw; // Suppress unused parameter warning
    if (index >= current_view.size()) return;
    
    auto entry = current_view[index];
    bool is_selected = (index == selected_index) && (focused_pane == FocusedPane::Main);
    
    // Get cached formatting or create new
    auto& cached = format_cache[entry];
    if (cached.needs_update) {
        update_format_cache(entry, cached, win_width);
    }
    
    // Move to line position
    wmove(win, y, 0);
    wclrtoeol(win);
    
    // Apply selection highlighting
    if (is_selected) {
        wattron(win, COLOR_PAIR(4));
        for (int i = 0; i < win_width; i++) {
            mvwaddch(win, y, i, ' ');
        }
    }
    
    // Draw the line content
    int col_x = 0;
    
    // Mark indicator
    if (entry->marked.load()) {
        if (!is_selected) wattron(win, COLOR_PAIR(8) | A_BOLD);
        mvwaddch(win, y, col_x, '*');
        if (!is_selected) wattroff(win, COLOR_PAIR(8) | A_BOLD);
    } else {
        mvwaddch(win, y, col_x, ' ');
    }
    col_x = 1;
    
    // Size
    if (is_selected) {
        wattron(win, COLOR_PAIR(4));
    } else {
        wattron(win, COLOR_PAIR(3));
    }
    mvwprintw(win, y, col_x, "%9s", cached.formatted_size.c_str());
    if (!is_selected) {
        wattroff(win, COLOR_PAIR(3));
    }
    col_x += 10;
    
    // Rest of the line formatting
    mvwprintw(win, y, col_x, " | ");
    col_x += 3;
    mvwprintw(win, y, col_x, "%5.1f%%", cached.percentage);
    col_x += 8;
    
    // Graph bar
    int bar_width = static_cast<int>(cached.percentage / 100.0 * 20);
    bar_width = std::min(bar_width, 20);
    if (is_selected) {
        for (int j = 0; j < bar_width; j++) {
            mvwaddch(win, y, col_x + j, '=');
        }
    } else {
        wattron(win, COLOR_PAIR(3));
        for (int j = 0; j < bar_width; j++) {
            mvwaddch(win, y, col_x + j, ACS_CKBOARD);
        }
        wattroff(win, COLOR_PAIR(3));
    }
    col_x += 20;
    
    // Modified time column (if enabled) - now after the bar
    if (show_mtime) {
        mvwprintw(win, y, col_x, " | ");
        col_x += 3;
        
        if (is_selected) {
            wattron(win, COLOR_PAIR(4));
        } else {
            wattron(win, COLOR_PAIR(2));
        }
        
        // Format the time
        // Convert file_time_type to time_t (C++17 compatible way)
        auto file_time_epoch = entry->last_modified.time_since_epoch();
        auto sys_time_epoch = std::chrono::duration_cast<std::chrono::system_clock::duration>(file_time_epoch);
        auto sys_time = std::chrono::system_clock::time_point(sys_time_epoch);
        auto time_t_val = std::chrono::system_clock::to_time_t(sys_time);
        
        std::tm* tm = std::localtime(&time_t_val);
        char time_buffer[20];
        if (tm) {
            std::strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M", tm);
        } else {
            std::strcpy(time_buffer, "----/--/-- --:--");
        }
        
        mvwprintw(win, y, col_x, "%16s", time_buffer);
        if (!is_selected) {
            wattroff(win, COLOR_PAIR(2));
        }
        col_x += 17;
    }
    
    // Entry count column (if enabled)
    if (show_count) {
        mvwprintw(win, y, col_x, " | ");
        col_x += 3;
        
        if (is_selected) {
            wattron(win, COLOR_PAIR(4));
        } else {
            wattron(win, COLOR_PAIR(2));
        }
        
        if (entry->entry_count.load() > 0) {
            mvwprintw(win, y, col_x, "%6llu", (unsigned long long)entry->entry_count.load());
        } else {
            mvwprintw(win, y, col_x, "     -");
        }
        
        if (!is_selected) {
            wattroff(win, COLOR_PAIR(2));
        }
        col_x += 7;
    }
    
    // Name
    mvwprintw(win, y, col_x, " | ");
    col_x += 3;
    
    if (entry->is_symlink && !is_selected) {
        wattron(win, COLOR_PAIR(9));
    } else if (entry->is_directory && !is_selected) {
        wattron(win, COLOR_PAIR(1) | A_BOLD);
    }
    
    mvwprintw(win, y, col_x, "%s", cached.formatted_name.c_str());
    
    if ((entry->is_symlink || entry->is_directory) && !is_selected) {
        wattroff(win, COLOR_PAIR(entry->is_symlink ? 9 : 1) | (entry->is_directory ? A_BOLD : 0));
    }
    
    if (is_selected) {
        wattroff(win, COLOR_PAIR(4));
    }
}

void InteractiveUI::update_format_cache(std::shared_ptr<Entry> entry, CachedEntry& cached, int win_width) {
    cached.formatted_size = format_size(entry->size, config.format);
    cached.percentage = (current_dir->size > 0) ? 
        (static_cast<double>(entry->size.load()) / current_dir->size.load() * 100.0) : 0.0;
    
    std::string name = entry->path.filename().string();
    if (name.empty()) name = entry->path.string();
    
    if (entry->is_symlink) {
        cached.formatted_name = " " + name + " -> " + entry->symlink_target.string();
    } else if (entry->is_directory) {
        cached.formatted_name = "/" + name;
    } else {
        cached.formatted_name = " " + name;
    }
    
    // Calculate available width for name based on enabled columns
    int used_width = 1 + 10 + 3 + 8 + 3 + 20;  // mark + size + sep + % + sep + bar
    if (show_mtime) {
        used_width += 3 + 17;  // separator + mtime
    }
    if (show_count) {
        used_width += 3 + 7;  // separator + count
    }
    used_width += 3;  // final separator before name
    
    int available_width = win_width - used_width;
    if (cached.formatted_name.length() > static_cast<size_t>(available_width) && available_width > 3) {
        cached.formatted_name = "..." + 
            cached.formatted_name.substr(cached.formatted_name.length() - available_width + 3);
    }
    
    cached.needs_update = false;
}

void InteractiveUI::update_status_line(WINDOW* win, int height, int width) {
    (void)width; // Suppress unused parameter warning
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
    
    wattron(win, A_REVERSE);
    wmove(win, height - 2, 0);
    wclrtoeol(win);
    mvwprintw(win, height - 2, 1, "%s", sort_str.c_str());
    wattroff(win, A_REVERSE);
}

void InteractiveUI::draw_help(WINDOW* win) {
    int help_y = getmaxy(win) / 2 - 12;
    int help_x = getmaxx(win) / 2 - 40;
    
    // Draw help background
    wattron(win, COLOR_PAIR(7));
    for (int i = 0; i < 24; i++) {
        mvwhline(win, help_y + i, help_x, ' ', 80);
    }
    
    // Title
    wattron(win, A_BOLD);
    mvwprintw(win, help_y + 1, help_x + 35, "HELP");
    wattroff(win, A_BOLD);
    
    int y = help_y + 3;
    int left_col = help_x + 2;
    int right_col = help_x + 40;
    
    // Navigation section
    mvwprintw(win, y++, left_col, "Navigation:");
    mvwprintw(win, y, left_col + 2, "↑/k");
    mvwprintw(win, y++, left_col + 20, "Move up");
    mvwprintw(win, y, left_col + 2, "↓/j");
    mvwprintw(win, y++, left_col + 20, "Move down");
    mvwprintw(win, y, left_col + 2, "→/l/Enter");
    mvwprintw(win, y++, left_col + 20, "Enter directory");
    mvwprintw(win, y, left_col + 2, "←/h/u");
    mvwprintw(win, y++, left_col + 20, "Go back");
    mvwprintw(win, y, left_col + 2, "O");
    mvwprintw(win, y++, left_col + 20, "Open with system");
    mvwprintw(win, y, left_col + 2, "Tab");
    mvwprintw(win, y++, left_col + 20, "Switch to mark pane");
    
    y++;
    
    // Sorting section
    mvwprintw(win, y++, left_col, "Sorting:");
    mvwprintw(win, y, left_col + 2, "s");
    mvwprintw(win, y++, left_col + 20, "By size");
    mvwprintw(win, y, left_col + 2, "n");
    mvwprintw(win, y++, left_col + 20, "By name");
    mvwprintw(win, y, left_col + 2, "m");
    mvwprintw(win, y++, left_col + 20, "By modified time");
    mvwprintw(win, y, left_col + 2, "c");
    mvwprintw(win, y++, left_col + 20, "By entry count");
    
    // Marking section (right column)
    y = help_y + 3;
    mvwprintw(win, y++, right_col, "Marking:");
    mvwprintw(win, y, right_col + 2, "space");
    mvwprintw(win, y++, right_col + 20, "Toggle mark");
    mvwprintw(win, y, right_col + 2, "d");
    mvwprintw(win, y++, right_col + 20, "Mark & move down");
    mvwprintw(win, y, right_col + 2, "a");
    mvwprintw(win, y++, right_col + 20, "Toggle all");
    mvwprintw(win, y, right_col + 2, "d");
    mvwprintw(win, y++, right_col + 20, "Delete marked");
    
    y += 3;
    
    // Display section (right column)
    mvwprintw(win, y++, right_col, "Display:");
    mvwprintw(win, y, right_col + 2, "M");
    mvwprintw(win, y++, right_col + 20, "Toggle mtime");
    mvwprintw(win, y, right_col + 2, "C");
    mvwprintw(win, y++, right_col + 20, "Toggle count");
    mvwprintw(win, y, right_col + 2, "/");
    mvwprintw(win, y++, right_col + 20, "Glob search");
    mvwprintw(win, y, right_col + 2, "r/R");
    mvwprintw(win, y++, right_col + 20, "Refresh");
    
    // Additional navigation keys (left column continued)
    y = help_y + 17;
    mvwprintw(win, y, left_col + 2, "Page Up/Ctrl+u");
    mvwprintw(win, y++, left_col + 20, "Move up 10");
    mvwprintw(win, y, left_col + 2, "Page Down/Ctrl+d");
    mvwprintw(win, y++, left_col + 20, "Move down 10");
    mvwprintw(win, y, left_col + 2, "Home/H");
    mvwprintw(win, y++, left_col + 20, "Go to top");
    mvwprintw(win, y, left_col + 2, "End/G");
    mvwprintw(win, y++, left_col + 20, "Go to bottom");
    
    // Close help message
    mvwprintw(win, help_y + 22, help_x + 20, "Press any key to close help");
    
    wattroff(win, COLOR_PAIR(7));
}

// Input handling implementations
bool InteractiveUI::handle_key(int ch) {
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
            check_mark_pane_visibility();
            break;
            
        case 'a':
        case 'A':
            toggle_all_marks();
            check_mark_pane_visibility();
            needs_full_redraw = true;
            break;
            
        case 'd':
            if (has_marked_items()) {
                delete_marked_entries();
                check_mark_pane_visibility();
                needs_full_redraw = true;
            } else if (selected_index < current_view.size()) {
                current_view[selected_index]->marked = true;
                mark_pane.update_marked_items(roots);  // Update mark pane
                navigate_down();
                check_mark_pane_visibility();
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
            
        default:
            break;
    }
    
    return true;  // Continue running
}

bool InteractiveUI::handle_mark_pane_key(int ch) {
    switch (ch) {
        case KEY_UP:
        case 'k':
            mark_pane.navigate_up();
            if (!mark_pane.is_empty()) {
                mark_pane.draw(mark_win, getmaxy(mark_win), getmaxx(mark_win));
            }
            break;
            
        case KEY_DOWN:
        case 'j':
            mark_pane.navigate_down();
            if (!mark_pane.is_empty()) {
                mark_pane.draw(mark_win, getmaxy(mark_win), getmaxx(mark_win));
            }
            break;
            
        case ' ':
        case 'x':
        case 'd':
            mark_pane.remove_selected();
            if (mark_pane.is_empty()) {
                focused_pane = FocusedPane::Main;
                mark_pane.set_focus(false);
                update_window_layout();
                needs_full_redraw = true;
            } else {
                mark_pane.draw(mark_win, getmaxy(mark_win), getmaxx(mark_win));
            }
            break;
            
        case 'a':
        case 'A':
            mark_pane.remove_all();
            focused_pane = FocusedPane::Main;
            mark_pane.set_focus(false);
            update_window_layout();
            needs_full_redraw = true;
            break;
            
        case 'q':
        case 'Q':
        case 27:  // ESC
            focused_pane = FocusedPane::Main;
            mark_pane.set_focus(false);
            needs_full_redraw = true;
            break;
            
        default:
            break;
    }
    
    return true;  // Continue running
}

void InteractiveUI::handle_glob_search(int ch) {
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

bool InteractiveUI::has_any_marked_items() {
    return has_marked_recursive(current_dir);
}

bool InteractiveUI::has_marked_recursive(std::shared_ptr<Entry> root) {
    if (root->marked.load()) {
        return true;
    }
    
    if (root->is_directory && !root->is_symlink) {
        std::lock_guard<std::mutex> lock(root->children_mutex);
        for (auto& child : root->children) {
            if (has_marked_recursive(child)) {
                return true;
            }
        }
    }
    return false;
}

void InteractiveUI::print_marked_paths() {
    std::vector<std::shared_ptr<Entry>> marked_entries;
    
    for (auto& root : roots) {
        collect_marked_entries(root, marked_entries);
    }
    
    for (auto& entry : marked_entries) {
        std::cout << entry->path << "\n";
    }
}

// Additional helper method implementations
void InteractiveUI::enter_directory() {
    if (selected_index < current_view.size()) {
        auto selected = current_view[selected_index];
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

void InteractiveUI::exit_directory() {
    if (navigation_stack.size() > 1) {
        navigation_stack.pop_back();
        current_dir = navigation_stack.back();
        update_view();
        selected_index = 0;
        view_offset = 0;
        needs_full_redraw = true;
    }
}

void InteractiveUI::toggle_mark() {
    if (selected_index < current_view.size()) {
        auto entry = current_view[selected_index];
        entry->marked = !entry->marked.load();
        
        // Update the mark pane with current marked items
        mark_pane.update_marked_items(roots);
    }
}

void InteractiveUI::toggle_all_marks() {
    bool any_marked = has_marked_items();
    for (auto& entry : current_view) {
        entry->marked = !any_marked;
    }
    
    // Update the mark pane with current marked items
    mark_pane.update_marked_items(roots);
}

bool InteractiveUI::has_marked_items() {
    for (const auto& entry : current_view) {
        if (entry->marked.load()) {
            return true;
        }
    }
    return false;
}

void InteractiveUI::check_mark_pane_visibility() {
    bool should_show_mark_pane = !mark_pane.is_empty();
    bool is_showing_mark_pane = (mark_win != nullptr);
    
    if (should_show_mark_pane != is_showing_mark_pane) {
        update_window_layout();
        needs_full_redraw = true;
    }
}

void InteractiveUI::start_glob_search() {
    glob_search_active = true;
    glob_pattern.clear();
}

void InteractiveUI::perform_glob_search() {
    if (glob_pattern.empty()) return;
    
    std::vector<std::shared_ptr<Entry>> matches;
    search_entries(current_dir, glob_pattern, matches);
    
    if (!matches.empty()) {
        auto search_results = std::make_shared<Entry>("[Search Results]");
        search_results->is_directory = true;
        search_results->children = matches;
        
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

void InteractiveUI::search_entries(std::shared_ptr<Entry> root, const std::string& pattern, 
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

void InteractiveUI::open_selected() {
    if (selected_index < current_view.size()) {
        auto selected = current_view[selected_index];
        std::string command;
        
#ifdef __linux__
        command = "xdg-open ";
#elif __APPLE__
        command = "open ";
#else
        return;
#endif
        
        command += "\"" + selected->path.string() + "\" 2>/dev/null &";
        system(command.c_str());
    }
}

void InteractiveUI::delete_marked_entries() {
    std::vector<std::shared_ptr<Entry>> marked_entries;
    collect_marked_entries(current_dir, marked_entries);
    
    if (marked_entries.empty()) return;
    
    // Create confirmation dialog
    int dialog_height = 8;
    int dialog_width = 60;
    int dialog_y = (LINES - dialog_height) / 2;
    int dialog_x = (COLS - dialog_width) / 2;
    
    WINDOW* dialog = newwin(dialog_height, dialog_width, dialog_y, dialog_x);
    box(dialog, 0, 0);
    
    // Display warning message
    mvwprintw(dialog, 1, 2, "WARNING: About to delete %zu item(s)", marked_entries.size());
    mvwprintw(dialog, 2, 2, "This action cannot be undone!");
    mvwprintw(dialog, 4, 2, "Type YES and press Enter to confirm deletion:");
    mvwprintw(dialog, 5, 2, ">");
    
    wrefresh(dialog);
    
    // Get user confirmation
    echo();
    char confirmation[10];
    mvwgetnstr(dialog, 5, 4, confirmation, 9);
    noecho();
    
    delwin(dialog);
    touchwin(stdscr);
    refresh();
    
    // Check if user typed "YES"
    if (std::string(confirmation) != "YES") {
        return;  // User didn't confirm, abort deletion
    }
    
    // Proceed with deletion
    for (auto& entry : marked_entries) {
        try {
            if (entry->is_directory && !entry->is_symlink) {
                fs::remove_all(entry->path);
            } else {
                fs::remove(entry->path);
            }
            entry->marked = false;
        } catch (...) {
            // Continue with other files
        }
    }
    
    mark_pane.remove_all();
    refresh_all();
}

void InteractiveUI::collect_marked_entries(std::shared_ptr<Entry> root, 
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

void InteractiveUI::refresh_selected() {
    if (selected_index < current_view.size()) {
        auto selected = current_view[selected_index];
        if (selected->is_directory && !selected->is_symlink) {
            clear();
            mvprintw(LINES / 2, COLS / 2 - 10, "Refreshing...");
            refresh();
            
            WorkStealingThreadPool pool(config.thread_count);
            OptimizedScanner scanner(pool, config);
            
            {
                std::lock_guard<std::mutex> lock(selected->children_mutex);
                selected->children.clear();
            }
            
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

void InteractiveUI::refresh_all() {
    clear();
    mvprintw(LINES / 2, COLS / 2 - 10, "Refreshing all...");
    refresh();
    
    WorkStealingThreadPool pool(config.thread_count);
    OptimizedScanner scanner(pool, config);
    
    if (roots.size() > 1) {
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
        roots = scanner.scan({roots[0]->path});
        navigation_stack.clear();
        current_dir = roots[0];
        navigation_stack.push_back(current_dir);
    }
    
    update_view();
    selected_index = 0;
    view_offset = 0;
}

void InteractiveUI::handle_resize() {
    // Clear and refresh the standard screen
    clear();
    refresh();
    
    // Delete existing windows
    if (main_win) {
        delwin(main_win);
        main_win = nullptr;
    }
    if (mark_win) {
        delwin(mark_win);
        mark_win = nullptr;
    }
    
    // Recreate windows with new dimensions
    update_window_layout();
    
    // Update cache dimensions
    line_cache.clear();  // Clear cache to force rebuild with new dimensions
    
    // Force full redraw
    needs_full_redraw = true;
    
    // Adjust view offset if necessary
    int visible_lines = getmaxy(main_win) - 2;
    if (view_offset > 0 && selected_index - view_offset >= static_cast<size_t>(visible_lines - 1)) {
        view_offset = std::max(static_cast<size_t>(0), 
                               selected_index - static_cast<size_t>(visible_lines) + 2);
    }
}

void InteractiveUI::sort_by_size() {
    sort_mode = (sort_mode == SortMode::SIZE_DESC) ? 
                SortMode::SIZE_ASC : SortMode::SIZE_DESC;
    apply_sort();
}

void InteractiveUI::sort_by_name() {
    sort_mode = (sort_mode == SortMode::NAME_ASC) ? 
                SortMode::NAME_DESC : SortMode::NAME_ASC;
    apply_sort();
}

void InteractiveUI::sort_by_time() {
    sort_mode = (sort_mode == SortMode::TIME_DESC) ? 
                SortMode::TIME_ASC : SortMode::TIME_DESC;
    apply_sort();
}

void InteractiveUI::sort_by_count() {
    sort_mode = (sort_mode == SortMode::COUNT_DESC) ? 
                SortMode::COUNT_ASC : SortMode::COUNT_DESC;
    apply_sort();
}

void InteractiveUI::delete_marked_from_pane() {
    auto marked_entries = mark_pane.get_all_marked();
    
    if (marked_entries.empty()) return;
    
    for (auto& entry : marked_entries) {
        try {
            if (entry->is_directory && !entry->is_symlink) {
                fs::remove_all(entry->path);
            } else {
                fs::remove(entry->path);
            }
            entry->marked = false;
        } catch (...) {
            // Continue with other files
        }
    }
    
    mark_pane.remove_all();
    refresh_all();
}

void InteractiveUI::remove_from_parent(std::shared_ptr<Entry> entry) {
    // Simplified implementation
    entry->size = 0;
    entry->entry_count = 0;
}
