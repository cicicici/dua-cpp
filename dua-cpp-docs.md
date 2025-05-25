# DUA C++ Enhanced Implementation Documentation

## Overview

This enhanced C++ implementation of dua-cli brings the reimplementation much closer to feature parity with the original Rust version. The enhancements focus on matching the functionality, user experience, and performance characteristics of the original while maintaining the benefits of a C++ implementation.

## New Features Added

### 1. **Subcommand Structure**
The enhanced version now properly implements the subcommand structure from the original:
- `dua i` or `dua interactive` - Launch interactive mode
- `dua a` or `dua aggregate` - Show aggregated disk usage (default)
- Default behavior intelligently chooses based on terminal detection

### 2. **Glob Search Functionality** 
Pressing `/` in interactive mode now activates glob search, allowing you to search for files and directories using git-style glob patterns. The search results are displayed in a virtual directory view, making it easy to navigate through matches. This matches the original dua-cli's search functionality.

### 3. **Refresh Capabilities**
- `r` - Refresh the selected directory
- `R` - Refresh all entries in the current view
This allows you to update disk usage information without restarting the application, particularly useful when monitoring changing directories.

### 4. **Entry Count Support**
- `C` key toggles the entry count column display
- `c` key cycles sorting by entry count (ascending/descending)
- Shows the total number of files and directories within each entry

### 5. **Modified Time Column**
- `M` key toggles the modified time column display
- `m` key cycles sorting by modification time
- Displays last modification timestamp in a readable format

### 6. **Open File/Directory Integration**
- `Shift+O` opens the selected file or directory with the system default application
- Uses `xdg-open` on Linux and `open` on macOS
- Provides seamless integration with the desktop environment

### 7. **Multiple Root Paths**
The enhanced version now supports analyzing multiple directories simultaneously:
```bash
dua /home /var /tmp
```
When multiple paths are provided, a virtual root is created to display all paths together, similar to the original dua-cli.

### 8. **Hard Link Detection**
- `--count-hard-links` flag controls whether hard links are counted multiple times
- By default, hard links are counted only once to provide accurate disk usage
- Uses inode tracking to detect hard links across the filesystem

### 9. **Apparent Size vs Disk Usage**
- `--apparent-size` flag shows file sizes instead of disk usage
- By default, shows actual disk usage (block-aligned sizes)
- Provides accurate representation of space consumed on disk

### 10. **Filesystem Boundary Control**
- `--stay-on-filesystem` prevents crossing filesystem boundaries
- Useful for analyzing single partitions without including mounted filesystems
- Automatically detects device IDs to enforce boundaries

### 11. **Directory Ignore Support**
- `--ignore-dirs` flag allows specifying directories to skip during traversal
- Can be specified multiple times for multiple directories
- Paths are canonicalized for accurate matching

### 12. **Enhanced Status Display**
The status bar now shows:
- Current sort mode with detailed description
- Total disk usage
- Number of marked items and their total size
- Entry count information
- Scan progress and timing

### 13. **Improved Performance**
- Special handling for macOS (3 threads by default, as in the original)
- Configurable thread pool size with `--threads` option
- Optimized directory traversal with batching
- Memory-efficient entry storage

## Command-Line Interface

### Subcommands
```bash
dua [SUBCOMMAND] [OPTIONS] [PATH...]
```

**Subcommands:**
- `i, interactive` - Launch the terminal user interface
- `a, aggregate` - Show aggregated disk usage (default for non-TTY)

### Options
- `-h, --help` - Show help message
- `-A, --apparent-size` - Display apparent size instead of disk usage
- `-l, --count-hard-links` - Count hard links each time they are seen
- `-x, --stay-on-filesystem` - Don't cross filesystem boundaries  
- `-d, --depth N` - Maximum depth to traverse
- `-t, --top N` - Show only top N entries by size
- `-f, --format FMT` - Output format: metric, binary, bytes, gb, gib, mb, mib
- `-j, --threads N` - Number of threads (0 = auto-detect)
- `-i, --ignore-dirs DIR` - Directories to ignore (can be repeated)
- `--no-entry-check` - Skip entry validation for better performance
- `--no-colors` - Disable colored output

## Interactive Mode Enhancements

### Navigation
- Arrow keys or vim-style (h,j,k,l) for movement
- Enter/o/l to enter directories
- Backspace/u/h to go back
- Shift+O to open with system default application

### Marking and Operations
- Space to toggle mark on current item
- `d` to mark and move down (quick marking)
- `a` to toggle all marks
- `d` on marked items to delete them

### Search and Filter
- `/` to activate glob search
- Type pattern and press Enter to search
- ESC to cancel search
- Search results shown in virtual directory

### Sorting
- `s` - Toggle sort by size
- `n` - Toggle sort by name
- `m` - Toggle sort by modified time
- `c` - Toggle sort by entry count

### Display Options
- `M` - Toggle modified time column
- `C` - Toggle entry count column
- `?` - Show/hide help

### Refresh
- `r` - Refresh selected directory
- `R` - Refresh all entries in view

## Implementation Details

### Thread Pool Optimization
The thread pool now includes platform-specific optimizations:
```cpp
#ifdef __APPLE__
    threads = 3;  // Optimal for macOS as per original dua-cli
#endif
```

### Hard Link Tracking
Hard links are tracked using inode and device ID:
```cpp
struct InodeKey {
    dev_t device;
    ino_t inode;
};
```

### Size Calculation
Two size metrics are maintained:
- `apparent_size` - The file size as reported by the filesystem
- `size` - The actual disk usage (block-aligned)

### Glob Pattern Matching
Glob patterns are converted to regex for flexible matching:
- `*` matches any sequence of characters
- `?` matches any single character
- Pattern matching is case-insensitive

## Building the Enhanced Version

### Requirements
- C++17 compatible compiler
- ncurses library
- POSIX-compliant system

### Compilation
```bash
# Standard build
g++ -std=c++17 -O3 -Wall -Wextra -pthread dua_enhanced.cpp -o dua -lncurses

# Debug build
g++ -std=c++17 -g -O0 -DDEBUG -Wall -Wextra -pthread dua_enhanced.cpp -o dua -lncurses

# Static build (for portability)
g++ -std=c++17 -O3 -Wall -Wextra -pthread -static dua_enhanced.cpp -o dua -lncurses
```

### Platform-Specific Notes

#### Linux
Requires the following headers:
- `<sys/stat.h>` - For inode information
- `<linux/fs.h>` - For filesystem operations

#### macOS
- Default thread count set to 3 for optimal performance
- Uses `stat` for inode information
- `open` command for file opening

## Usage Examples

### Basic Usage
```bash
# Analyze current directory
dua

# Analyze specific directory
dua /var/log

# Analyze multiple directories
dua /home /var /tmp
```

### Interactive Mode
```bash
# Interactive mode with current directory
dua i

# Interactive mode with specific paths
dua i /usr /opt

# Interactive mode without entry checking (faster)
dua i --no-entry-check /
```

### Aggregate Mode
```bash
# Show disk usage with specific format
dua a --format binary /home

# Show apparent sizes
dua a --apparent-size /var

# Stay within filesystem boundaries
dua a --stay-on-filesystem /

# Ignore certain directories
dua a --ignore-dirs /proc --ignore-dirs /sys /
```

### Advanced Usage
```bash
# Count hard links separately
dua -l /usr

# Limit traversal depth
dua -d 3 /

# Show only top 20 entries
dua -t 20 /home

# Use specific thread count
dua -j 8 /large/directory
```

## Performance Considerations

1. **Entry Checking**: Use `--no-entry-check` for faster navigation in interactive mode when dealing with slow filesystems.

2. **Thread Count**: The default auto-detection works well, but you can tune it:
   - Use fewer threads on systems with slow I/O
   - Use more threads on systems with fast SSDs and many cores

3. **Hard Link Counting**: Disabling hard link detection with `-l` can speed up scanning but may show inflated sizes.

4. **Filesystem Boundaries**: Using `--stay-on-filesystem` can significantly reduce scan time when multiple filesystems are mounted.

## Differences from Original dua-cli

While this implementation aims for feature parity, some differences remain:

1. **TUI Backend**: Uses ncurses instead of crossterm
2. **Trash Support**: Not yet implemented (only permanent deletion)
3. **Configuration Files**: No support for configuration files yet
4. **Export Formats**: Limited to console output (no JSON export)

## Future Enhancements

Potential improvements for full feature parity:

1. **Trash/Recycle Bin Support**: Implement cross-platform trash functionality
2. **Configuration File**: Support for .dua.toml configuration
3. **More Export Formats**: JSON, CSV output for aggregate mode
4. **Windows Support**: Port to Windows using PDCurses
5. **Extended Attributes**: Show extended filesystem attributes
6. **Compression Ratios**: Detect and display compressed file ratios

## Contributing

When contributing to this C++ implementation:

1. Maintain C++17 compatibility
2. Follow the existing code style
3. Add appropriate error handling
4. Test on multiple platforms
5. Update documentation for new features

## Testing

Run comprehensive tests:
```bash
# Test basic functionality
./dua /tmp

# Test interactive mode
./dua i /usr /var

# Test with various options
./dua --apparent-size --count-hard-links /home

# Test glob search
# In interactive mode, press '/' and search for "*.log"

# Test refresh functionality  
# In interactive mode, create/delete files in another terminal
# Press 'r' to refresh selected or 'R' to refresh all
```

## Troubleshooting

### Common Issues

1. **"Error: Path does not exist"**
   - Verify the path is correct
   - Check permissions

2. **Slow Performance**
   - Try using `--no-entry-check` in interactive mode
   - Adjust thread count with `-j`
   - Use `--ignore-dirs` for problematic directories

3. **High Memory Usage**
   - Normal for millions of files
   - Consider using `--depth` to limit traversal

4. **Incomplete Results**
   - Check for permission errors in output
   - May need to run with elevated privileges for system directories

### Debug Mode

Compile with debug flags for troubleshooting:
```bash
g++ -std=c++17 -g -O0 -DDEBUG -Wall -Wextra -pthread dua_enhanced.cpp -o dua_debug -lncurses
```

Then run with a debugger:
```bash
gdb ./dua_debug
```

## Conclusion

This enhanced C++ implementation brings dua-cli much closer to the original Rust version while maintaining the performance benefits of C++. The implementation focuses on providing a familiar user experience for those switching from the original while taking advantage of C++ features for efficiency and portability.