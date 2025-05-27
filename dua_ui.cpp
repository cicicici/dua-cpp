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
    std::string title = " Mark Pane ";
    mvwprintw(win, 0, (width - title.length()) / 2, "%s", title.c_str());
    
    // Draw tabs
    draw_tabs(win, width);
    
    // Draw content based on current tab
    if (tab_manager.get_current_tab() == MarkPaneTab::QUICKVIEW) {
        draw_quickview(win, height, width);
    } else {
        draw_marked_files(win, height, width);
    }
    
    // Help text at bottom
    if (has_focus) {
        std::string help_text = (tab_manager.get_current_tab() == MarkPaneTab::QUICKVIEW) ?
            " 1/2 = switch tabs | Tab = back " :
            " 1/2 = tabs | x/d = remove | a = all ";
        wattron(win, A_BOLD);
        mvwprintw(win, height - 1, 2, "%s", help_text.c_str());
        wattroff(win, A_BOLD);
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

// Tab and quickview support methods for MarkPane
void MarkPane::switch_tab(int tab_number) {
    tab_manager.switch_to_tab(tab_number);
}

void MarkPane::activate_quickview(const fs::path& path) {
    tab_manager.activate_quickview(path);
}

void MarkPane::deactivate_quickview() {
    tab_manager.deactivate_quickview();
}

bool MarkPane::is_quickview_active() const {
    return tab_manager.is_quickview_active();
}

MarkPaneTab MarkPane::get_current_tab() const {
    return tab_manager.get_current_tab();
}

void MarkPane::draw_tabs(WINDOW* win, int width) {
    // Draw tab bar at the top
    wattron(win, A_REVERSE);
    mvwhline(win, 1, 1, ' ', width - 2);
    
    // Tab 1: Quick View
    std::string tab1 = " 1:Quick View ";
    if (tab_manager.get_current_tab() == MarkPaneTab::QUICKVIEW) {
        wattroff(win, A_REVERSE);
        wattron(win, A_BOLD);
    }
    mvwprintw(win, 1, 2, "%s", tab1.c_str());
    if (tab_manager.get_current_tab() == MarkPaneTab::QUICKVIEW) {
        wattroff(win, A_BOLD);
        wattron(win, A_REVERSE);
    }
    
    // Tab 2: Marked Files
    std::string tab2 = " 2:Marked Files ";
    if (tab_manager.get_current_tab() == MarkPaneTab::MARKED_FILES) {
        wattroff(win, A_REVERSE);
        wattron(win, A_BOLD);
    }
    mvwprintw(win, 1, 2 + tab1.length() + 1, "%s", tab2.c_str());
    if (tab_manager.get_current_tab() == MarkPaneTab::MARKED_FILES) {
        wattroff(win, A_BOLD);
    } else {
        wattroff(win, A_REVERSE);
    }
}

void MarkPane::draw_quickview(WINDOW* win, int height, int width) {
    if (!tab_manager.is_quickview_active()) {
        mvwprintw(win, height / 2, (width - 20) / 2, "No file selected");
        mvwprintw(win, height / 2 + 1, (width - 30) / 2, "Press 'i' on a file to preview");
        return;
    }
    
    const PreviewContent& preview = tab_manager.get_cached_preview();
    ScrollableView& scroll_view = tab_manager.get_scroll_view();
    
    // Update window size (subtract borders, header, and status line)
    int content_width = width - 4;  // 2 chars padding on each side
    int content_height = height - 5;  // Header, tabs, borders, and status line
    scroll_view.update_window_size(content_width, content_height);
    
    // Draw content within the visible window
    size_t start_y = scroll_view.get_visible_start_y();
    size_t end_y = scroll_view.get_visible_end_y();
    size_t start_x = scroll_view.get_visible_start_x();
    
    int draw_y = 3;  // Start after header and tabs
    
    for (size_t line_idx = start_y; line_idx < end_y && line_idx < preview.lines.size() && draw_y < height - 2; line_idx++) {
        const std::string& line = preview.lines[line_idx];
        
        // Clear the line first
        wmove(win, draw_y, 2);
        wclrtoeol(win);
        mvwhline(win, draw_y, width - 1, ACS_VLINE, 1);  // Restore right border
        
        // Draw the visible portion of the line
        if (start_x < line.length()) {
            std::string visible_part = line.substr(start_x, content_width);
            
            // Always draw character by character to preserve spaces
            for (size_t col = 0; col < visible_part.length() && col < static_cast<size_t>(content_width); col++) {
                // Highlight cursor position if on this line and focused
                if (has_focus && line_idx == scroll_view.cursor_y && start_x + col == scroll_view.cursor_x) {
                    wattron(win, A_REVERSE);
                    mvwaddch(win, draw_y, 2 + col, visible_part[col]);
                    wattroff(win, A_REVERSE);
                } else {
                    mvwaddch(win, draw_y, 2 + col, visible_part[col]);
                }
            }
        }
        
        // Draw cursor on empty lines only
        if (has_focus && line_idx == scroll_view.cursor_y && line.empty()) {
            // For empty lines, show cursor at position 0
            if (scroll_view.cursor_x == 0 && start_x == 0) {
                wattron(win, A_REVERSE);
                mvwaddch(win, draw_y, 2, ' ');
                wattroff(win, A_REVERSE);
            }
        }
        
        draw_y++;
    }
    
    // Draw scroll indicators if needed
    if (preview.lines.size() > static_cast<size_t>(content_height) || 
        scroll_view.max_line_length > static_cast<size_t>(content_width)) {
        // Vertical scrollbar
        if (preview.lines.size() > static_cast<size_t>(content_height)) {
            // Pass height-2 to account for the status line
            draw_scrollbar(win, height-2, scroll_view.view_offset_y, preview.lines.size(), content_height);
        }
        
        // Horizontal scroll indicator in status line
        if (scroll_view.view_offset_x > 0 || scroll_view.content_width > static_cast<size_t>(content_width)) {
            std::string h_scroll = "[" + std::to_string(start_x + 1) + "-" + 
                                 std::to_string(start_x + content_width) + "]";
            mvwprintw(win, height - 2, width - h_scroll.length() - 2, "%s", h_scroll.c_str());
        }
    }
    
    // Show cursor position in status line
    if (has_focus) {
        std::string cursor_info = "Line " + std::to_string(scroll_view.cursor_y + 1) + "/" + 
                                 std::to_string(preview.lines.size()) + 
                                 " Col " + std::to_string(scroll_view.cursor_x + 1);
        mvwprintw(win, height - 2, 2, "%s", cursor_info.c_str());
    }
}

void MarkPane::draw_marked_files(WINDOW* win, int height, int width) {
    if (marked_items.empty()) {
        mvwprintw(win, height / 2, (width - 20) / 2, "No marked items");
        return;
    }
    
    // Draw items (leave space for header, tabs, bottom info, and border)
    int visible_items = height - 5;
    int y = 3;
    
    for (size_t i = view_offset; i < marked_items.size() && y < height - 2; i++) {
        bool is_selected = (has_focus && i == selected_index);
        
        if (is_selected) {
            wattron(win, A_REVERSE);
        }
        
        // Clear line
        mvwhline(win, y, 1, ' ', width - 2);
        
        // Format entry
        std::string size_str = format_size(marked_sizes[i], config.format);
        std::string path_str = marked_paths[i];
        
        // Fixed column widths
        const int size_col_width = 10;   // Fixed width for size column
        const int separator_width = 3;   // " | "
        const int path_start = 2 + size_col_width + separator_width;
        
        // Truncate path if too long
        int max_path_len = width - path_start - 2;
        if (max_path_len > 0 && static_cast<int>(path_str.length()) > max_path_len) {
            path_str = "..." + path_str.substr(path_str.length() - max_path_len + 3);
        }
        
        // Draw with colors
        auto& item = marked_items[i];
        
        // Size in green (right-aligned in fixed width column)
        wattron(win, COLOR_PAIR(3));
        mvwprintw(win, y, 2, "%*s", size_col_width, size_str.c_str());
        wattroff(win, COLOR_PAIR(3));
        
        // Separator at fixed position
        mvwprintw(win, y, 2 + size_col_width, " | ");
        
        // Path with appropriate color at fixed position
        wmove(win, y, path_start);
        if (item->is_symlink) {
            wattron(win, COLOR_PAIR(9));  // Magenta for symlinks
        } else if (item->is_directory) {
            wattron(win, COLOR_PAIR(1) | A_BOLD);  // Cyan bold for directories
        }
        
        wprintw(win, "%s", path_str.c_str());
        
        if (item->is_symlink || item->is_directory) {
            wattroff(win, COLOR_PAIR(item->is_symlink ? 9 : 1) | 
                    (item->is_directory ? A_BOLD : 0));
        }
        
        if (is_selected) {
            wattroff(win, A_REVERSE);
        }
        
        y++;
    }
    
    // Draw scrollbar if needed
    if (static_cast<int>(marked_items.size()) > visible_items) {
        draw_scrollbar(win, height, view_offset, marked_items.size(), visible_items);
    }
    
    // Draw total information at bottom
    mvwhline(win, height - 2, 1, ACS_HLINE, width - 2);
    std::string total_info = "Total: " + std::to_string(marked_items.size()) + 
                            " items, " + format_size(total_size(), config.format);
    mvwprintw(win, height - 2, (width - total_info.length()) / 2, " %s ", total_info.c_str());
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
        init_pair(10, COLOR_BLACK, COLOR_BLUE);  // Unfocused selection
    }
    
    update_window_layout();
    
    bool running = true;
    int pending_move = 0;
    
    while (running) {
        // Draw main window
        if (needs_full_redraw) {
            draw_full();
            needs_full_redraw = false;
        } else {
            draw_differential();
        }
        
        // Draw mark pane if visible
        if (mark_win && (!mark_pane.is_empty() || mark_pane.is_quickview_active())) {
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
            
            if (ch == '\t' && (!mark_pane.is_empty() || mark_pane.is_quickview_active())) {
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
                    
                    // Update preview after batched movement
                    if (mark_pane.is_quickview_active() && selected_index < current_view.size()) {
                        mark_pane.activate_quickview(current_view[selected_index]->path);
                    }
                }
                
                if (ch == KEY_UP || ch == 'k') {
                    navigate_up();
                } else {
                    navigate_down();
                }
                
                // Update preview if quickview is active
                if (mark_pane.is_quickview_active() && selected_index < current_view.size()) {
                    mark_pane.activate_quickview(current_view[selected_index]->path);
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
                
                // Update preview after batched movement
                if (mark_pane.is_quickview_active() && selected_index < current_view.size()) {
                    mark_pane.activate_quickview(current_view[selected_index]->path);
                }
            }
            napms(50);  // Increased from 10ms to 50ms for lower CPU usage
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
    
    if (!mark_pane.is_empty() || mark_pane.is_quickview_active()) {
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
    mvwprintw(win, 0, 1, " Disk Usage Analyzer v%s [C++ Optimized]    (press ? for help)", DUA_VERSION);
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
    bool is_selected = (index == selected_index);
    bool has_focus = (focused_pane == FocusedPane::Main);
    
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
        if (has_focus) {
            wattron(win, COLOR_PAIR(4));  // Cyan background when focused
        } else {
            wattron(win, COLOR_PAIR(10));  // Blue background when not focused
        }
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
        if (has_focus) {
            wattron(win, COLOR_PAIR(4));
        } else {
            wattron(win, COLOR_PAIR(10));
        }
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
            if (has_focus) {
                wattron(win, COLOR_PAIR(4));
            } else {
                wattron(win, COLOR_PAIR(10));
            }
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
            if (has_focus) {
                wattron(win, COLOR_PAIR(4));
            } else {
                wattron(win, COLOR_PAIR(10));
            }
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
        if (has_focus) {
            wattroff(win, COLOR_PAIR(4));
        } else {
            wattroff(win, COLOR_PAIR(10));
        }
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
    
    // Format scan time
    std::string scan_time_str;
    if (scan_time_ms > 0) {
        if (scan_time_ms < 1000) {
            scan_time_str = "Scan time: " + std::to_string(scan_time_ms) + "ms";
        } else {
            scan_time_str = "Scan time: " + std::to_string(scan_time_ms / 1000.0).substr(0, 4) + "s";
        }
    }
    
    wattron(win, A_REVERSE);
    wmove(win, height - 2, 0);
    wclrtoeol(win);
    mvwprintw(win, height - 2, 1, "%s", sort_str.c_str());
    
    // Display scan time on the right side
    if (!scan_time_str.empty()) {
        int scan_time_x = width - static_cast<int>(scan_time_str.length()) - 1;
        if (scan_time_x > static_cast<int>(sort_str.length()) + 2) {
            mvwprintw(win, height - 2, scan_time_x, "%s", scan_time_str.c_str());
        }
    }
    
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
    mvwprintw(win, y, left_col + 2, "i");
    mvwprintw(win, y++, left_col + 20, "Quick view file");
    mvwprintw(win, y, left_col + 2, "I");
    mvwprintw(win, y++, left_col + 20, "Clear preview");
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
                mark_pane.update_marked_items(roots);  // Update immediately
                mark_pane.switch_tab(2);  // Switch to marked files tab
                navigate_down();
                check_mark_pane_visibility();
            }
            break;
            
        case 'O':  // Open with system
            open_selected();
            break;
            
        case 'i':  // Quick view
            if (selected_index < current_view.size()) {
                auto entry = current_view[selected_index];
                mark_pane.activate_quickview(entry->path);
                mark_pane.switch_tab(1);  // Switch to quickview tab
                check_mark_pane_visibility();  // Always check visibility
                needs_full_redraw = true;
            }
            break;
            
        case 'I':  // Clear preview
            mark_pane.deactivate_quickview();
            if (mark_pane.is_empty()) {
                // No marked files, close mark pane
                check_mark_pane_visibility();
                needs_full_redraw = true;
            } else {
                // Has marked files, switch to marked files tab
                mark_pane.switch_tab(2);
                needs_full_redraw = true;
            }
            break;
            
        case '/':  // Glob search
            start_glob_search();
            // Don't trigger full redraw - we want to keep the search prompt visible
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
    // Check if we're in quickview mode and handle navigation differently
    if (mark_pane.get_current_tab() == MarkPaneTab::QUICKVIEW && mark_pane.is_quickview_active()) {
        ScrollableView& scroll_view = mark_pane.get_tab_manager().get_scroll_view();
        bool needs_redraw = true;
        
        switch (ch) {
            case KEY_UP:
            case 'k':
                scroll_view.move_up();
                break;
                
            case KEY_DOWN:
            case 'j':
                scroll_view.move_down();
                break;
                
            case KEY_LEFT:
            case 'h':
                scroll_view.move_left();
                break;
                
            case KEY_RIGHT:
            case 'l':
                scroll_view.move_right();
                break;
                
            case KEY_PPAGE:  // Page Up
            case 'b':
                scroll_view.page_up();
                break;
                
            case KEY_NPAGE:  // Page Down
            case 'f':
                scroll_view.page_down();
                break;
                
            case KEY_HOME:
            case 'g':
                scroll_view.move_home();
                break;
                
            case KEY_END:
            case 'G':
                scroll_view.move_end();
                break;
                
            case '0':
                scroll_view.move_line_start();
                break;
                
            case '$':
                scroll_view.move_line_end();
                break;
                
            case '1':  // Switch to quickview tab
            case '2':  // Switch to marked files tab
                // Fall through to regular tab switching
                needs_redraw = true;
                break;
                
            case 'q':
            case 'Q':
            case 27:  // ESC
                // Fall through to regular handling
                needs_redraw = false;
                break;
                
            default:
                needs_redraw = false;
                break;
        }
        
        if (needs_redraw) {
            mark_pane.draw(mark_win, getmaxy(mark_win), getmaxx(mark_win));
            return true;
        } else if (ch == 'q' || ch == 'Q' || ch == 27 || ch == '1' || ch == '2') {
            // Continue to regular handling for these keys
        } else {
            return true;  // Consume other keys in quickview mode
        }
    }
    
    // Regular mark pane key handling
    switch (ch) {
        case '1':  // Switch to quickview tab
            mark_pane.switch_tab(1);
            mark_pane.draw(mark_win, getmaxy(mark_win), getmaxx(mark_win));
            break;
            
        case '2':  // Switch to marked files tab
            mark_pane.switch_tab(2);
            mark_pane.draw(mark_win, getmaxy(mark_win), getmaxx(mark_win));
            break;
            
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
    if (ch == 27) {  // ESC
        glob_search_active = false;
        needs_full_redraw = true;
        return;
    } else if (ch == '\n') {
        perform_glob_search();
        glob_search_active = false;
        needs_full_redraw = true;
        return;
    } else if (ch == KEY_BACKSPACE || ch == 127) {
        if (!glob_pattern.empty()) {
            glob_pattern.pop_back();
        }
    } else if (ch >= 32 && ch < 127) {
        glob_pattern += static_cast<char>(ch);
    }
    
    // Show search prompt with updated pattern
    move(LINES - 1, 0);
    clrtoeol();
    mvprintw(LINES - 1, 0, "Search: %s", glob_pattern.c_str());
    refresh();
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
        
        // Update mark pane immediately to ensure check_mark_pane_visibility works
        mark_pane.update_marked_items(roots);
        
        // Switch to marked files tab when marking state changes
        if (!mark_pane.is_empty()) {
            mark_pane.switch_tab(2);  // Switch to marked files tab
        }
    }
}

void InteractiveUI::toggle_all_marks() {
    bool any_marked = has_marked_items();
    for (auto& entry : current_view) {
        entry->marked = !any_marked;
    }
    
    // Update mark pane immediately
    mark_pane.update_marked_items(roots);
    
    // Switch to marked files tab when marking state changes
    if (!mark_pane.is_empty()) {
        mark_pane.switch_tab(2);  // Switch to marked files tab
    }
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
    bool should_show_mark_pane = !mark_pane.is_empty() || mark_pane.is_quickview_active();
    bool is_showing_mark_pane = (mark_win != nullptr);
    
    if (should_show_mark_pane != is_showing_mark_pane) {
        update_window_layout();
        needs_full_redraw = true;
    }
}

void InteractiveUI::start_glob_search() {
    glob_search_active = true;
    glob_pattern.clear();
    
    // Show search prompt immediately
    move(LINES - 1, 0);
    clrtoeol();
    mvprintw(LINES - 1, 0, "Search: ");
    refresh();
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
