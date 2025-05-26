# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Standard release build
make

# Debug build with symbols
make debug

# Static build (no dynamic dependencies)
make static

# Clean build artifacts
make clean

# Install to system (/usr/local/bin by default)
sudo make install

# Run tests
make test
```

## Linting and Code Quality

```bash
# Format code with clang-format
make format

# Run static analysis with clang-tidy
make lint

# Run all checks (lint + cppcheck)
make check
```

## Architecture Overview

This is a C++ implementation of dua-cli (Disk Usage Analyzer) with a modular architecture:

### Core Components

1. **dua_core.h/cpp** - Core scanning and data structures
   - `Entry` - File/directory representation with size, path, children
   - `WorkStealingThreadPool` - Parallel scanning with work stealing
   - `OptimizedScanner` - Directory traversal with batching (256 files/batch)
   - Platform-specific optimizations (3 threads on macOS M-series)

2. **dua_ui.h/cpp** - Interactive terminal UI
   - `InteractiveUI` - Main ncurses interface with differential rendering
   - `MarkPane` - Marked items management
   - Vim-style navigation (h,j,k,l)
   - Glob search with `/` key

3. **dua_enhanced.cpp** - Main entry point
   - Command-line parsing
   - Mode selection (interactive vs aggregate)
   - Subcommand handling (`i`/`interactive`, `a`/`aggregate`)

### Key Design Patterns

- **Parallel Scanning**: Uses atomic operations for thread-safe size accumulation
- **Differential Rendering**: Only redraws changed lines for UI performance
- **Memory Efficiency**: Pre-allocates child vectors, uses move semantics
- **Hard Link Detection**: Tracks inodes to avoid double-counting

### Platform Considerations

- Uses `std::filesystem` (C++17)
- ncurses for terminal UI
- Platform detection in Makefile for Linux/macOS/FreeBSD
- Signal handling for graceful shutdown

## Common Development Tasks

### Adding New Features

When adding features, follow the existing patterns:
- Scanner modifications go in `dua_core.cpp`
- UI changes go in `dua_ui.cpp`
- New command-line options in `dua_enhanced.cpp`

### Error Handling

Errors are logged to `std::cerr`:
```cpp
std::cerr << "Error: " << error_message << "\n";
```

### Thread Safety

- Use atomic operations for shared counters
- Mutex protection for shared data structures
- Work-stealing queue handles synchronization internally

## Claude Summary

DUA (Disk Usage Analyzer) is a fast, interactive terminal-based disk usage analyzer written in C++.
It's a reimplementation of the Rust-based dua-cli.

Key Features:
- Parallel directory scanning with work-stealing thread pool for speed
- Interactive ncurses UI with vim-style navigation (h,j,k,l)
- Multiple sort modes (size, name, mtime, count)
- Search functionality with glob patterns
- Safe file deletion with confirmations
- Hard link detection to avoid double-counting

Architecture:
- dua_core: Core scanning engine and data structures
- dua_ui: Interactive terminal interface
- dua_enhanced: Main entry point and CLI parsing
- Modular design with ~85% feature parity with the Rust original

Usage:
- dua - Interactive mode in current directory
- dua /path - Analyze specific path
- dua aggregate - Show top-level summary only
- Supports various options like -x (stay on filesystem), --ignore patterns, and format choices
