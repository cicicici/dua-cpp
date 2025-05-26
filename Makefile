# Makefile for Enhanced DUA C++ Implementation
# This Makefile is designed to be safe and will NEVER delete source files
# 
# Usage:
#   make              - Build the standard release version
#   make debug        - Build with debug symbols
#   make static       - Build statically linked version
#   make clean        - Remove build artifacts (NOT source files!)
#   make help         - Show all available targets

# ============================================================================
# CONFIGURATION SECTION
# ============================================================================

# Compiler configuration
# CXX is the C++ compiler to use. The ?= means it can be overridden from command line
CXX ?= g++

# Base compiler flags that are always used
CXXFLAGS_BASE = -std=c++17 -Wall -Wextra -pthread

# Platform detection for platform-specific settings
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

# ============================================================================
# PLATFORM-SPECIFIC SETTINGS
# ============================================================================

# Linux-specific settings
ifeq ($(UNAME_S),Linux)
    CXXFLAGS_PLATFORM = -D__linux__
    LDFLAGS_PLATFORM = -lncurses -lpthread
    # Check if we're using musl for fully static builds
    LIBC := $(shell ldd --version 2>&1 | grep -q musl && echo musl || echo glibc)
endif

# macOS-specific settings
ifeq ($(UNAME_S),Darwin)
    CXXFLAGS_PLATFORM = -D__APPLE__
    # macOS often has ncurses in a non-standard location
    CXXFLAGS_PLATFORM += -I/usr/local/opt/ncurses/include
    LDFLAGS_PLATFORM = -L/usr/local/opt/ncurses/lib -lncurses -lpthread
endif

# FreeBSD-specific settings
ifeq ($(UNAME_S),FreeBSD)
    CXXFLAGS_PLATFORM = -D__FreeBSD__
    LDFLAGS_PLATFORM = -lncurses -lpthread
endif

# ============================================================================
# BUILD CONFIGURATION OPTIONS
# ============================================================================

# These can be set from command line: make DEBUG=1
DEBUG ?= 0
STATIC ?= 0
LTO ?= 0        # Link-Time Optimization
NATIVE ?= 0     # CPU-specific optimizations

# ============================================================================
# CONDITIONAL FLAGS BASED ON BUILD OPTIONS
# ============================================================================

# Debug vs Release builds
ifeq ($(DEBUG),1)
    CXXFLAGS_BUILD = -g -O0 -DDEBUG
    TARGET_SUFFIX = _debug
else
    CXXFLAGS_BUILD = -O3 -DNDEBUG
    TARGET_SUFFIX =
endif

# Static linking
ifeq ($(STATIC),1)
    LDFLAGS_STATIC = -static
    TARGET_SUFFIX := $(TARGET_SUFFIX)_static
else
    LDFLAGS_STATIC =
endif

# Link-Time Optimization
ifeq ($(LTO),1)
    CXXFLAGS_LTO = -flto
    LDFLAGS_LTO = -flto
else
    CXXFLAGS_LTO =
    LDFLAGS_LTO =
endif

# Native CPU optimizations
ifeq ($(NATIVE),1)
    CXXFLAGS_NATIVE = -march=native -mtune=native
else
    CXXFLAGS_NATIVE =
endif

# ============================================================================
# VERSION INFORMATION
# ============================================================================

VERSION = 1.3.0
GIT_HASH = $(shell git rev-parse --short HEAD 2>/dev/null || echo "unknown")
BUILD_DATE = $(shell date +%Y-%m-%d)

# Add version info to build
CXXFLAGS_VERSION = -DDUA_VERSION=\"$(VERSION)\" \
                   -DGIT_HASH=\"$(GIT_HASH)\" \
                   -DBUILD_DATE=\"$(BUILD_DATE)\"

# ============================================================================
# FINAL FLAGS ASSEMBLY
# ============================================================================

# Combine all the flags
CXXFLAGS = $(CXXFLAGS_BASE) $(CXXFLAGS_PLATFORM) $(CXXFLAGS_BUILD) \
           $(CXXFLAGS_LTO) $(CXXFLAGS_NATIVE) $(CXXFLAGS_VERSION)

LDFLAGS = $(LDFLAGS_PLATFORM) $(LDFLAGS_STATIC) $(LDFLAGS_LTO)

# ============================================================================
# FILE DEFINITIONS
# ============================================================================

# Source files - IMPORTANT: These are your precious source files!
# The Makefile will NEVER delete these
SOURCES = dua_enhanced.cpp dua_core.cpp dua_ui.cpp dua_quickview.cpp

# Object files - These are temporary build products that can be safely deleted
OBJECTS = $(SOURCES:.cpp=.o)

# Dependency files for automatic header dependency tracking
DEPS = $(SOURCES:.cpp=.d)

# Target executable name
TARGET_BASE = dua
TARGET = $(TARGET_BASE)$(TARGET_SUFFIX)

# ============================================================================
# INSTALLATION PATHS
# ============================================================================

PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin
MANDIR = $(PREFIX)/share/man/man1

# ============================================================================
# PHONY TARGETS
# ============================================================================
# These are targets that don't create files with the same name

.PHONY: all clean debug release static install uninstall help
.PHONY: test test-interactive test-aggregate test-memory
.PHONY: format lint check show-config
.PHONY: push push-safe commit-push

# ============================================================================
# DEFAULT TARGET
# ============================================================================

# First target is the default when you just run 'make'
all: $(TARGET)

# ============================================================================
# MAIN BUILD RULES
# ============================================================================

# Rule to build the final executable
# $@ means "the target" ($(TARGET))
# $^ means "all the prerequisites" ($(OBJECTS))
$(TARGET): $(OBJECTS)
	@echo "Linking $@..."
	$(CXX) $(OBJECTS) -o $@ $(LDFLAGS)
	@echo "=================="
	@echo "Build successful!"
	@echo "Executable: $@"
	@echo "Platform: $(UNAME_S) $(UNAME_M)"
	@echo "Version: $(VERSION)"
	@echo "=================="

# Rule to compile .cpp files into .o files
# $< means "the first prerequisite" (the .cpp file)
# $@ means "the target" (the .o file)
%.o: %.cpp
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) -c $< -o $@

# ============================================================================
# CONVENIENCE BUILD TARGETS
# ============================================================================

# Debug build - includes debugging symbols
debug:
	@$(MAKE) DEBUG=1

# Optimized release build
release:
	@$(MAKE) DEBUG=0 LTO=1 NATIVE=1

# Static build - no dynamic library dependencies
static:
	@$(MAKE) STATIC=1 LTO=1

# ============================================================================
# CLEAN TARGET - SAFE VERSION
# ============================================================================
# This is the most important part - it only deletes generated files,
# NEVER source files!

# Clean targets
.PHONY: clean
clean:
	@echo "Cleaning build artifacts..."
	rm -f $(OBJECTS)
	rm -f $(TARGET_BASE) $(TARGET_BASE)_debug $(TARGET_BASE)_static $(TARGET_BASE)_debug_static
	rm -f $(TARGET_BASE)_profile $(TARGET_BASE)_asan $(TARGET_BASE)_tsan
	rm -f dua_linux_static dua_macos_universal
	rm -f *.o *.d core *.core
	rm -f gmon.out
	@echo "Clean complete!"

# ============================================================================
# INSTALLATION TARGETS
# ============================================================================

install: $(TARGET)
	@echo "Installing to $(BINDIR)..."
	# Create directory if it doesn't exist
	install -d $(BINDIR)
	# Copy executable with proper permissions
	install -m 755 $(TARGET) $(BINDIR)/$(TARGET_BASE)
	@echo "Installation complete!"
	@echo "Installed to: $(BINDIR)/$(TARGET_BASE)"

uninstall:
	@echo "Removing $(BINDIR)/$(TARGET_BASE)..."
	rm -f $(BINDIR)/$(TARGET_BASE)
	@echo "Uninstallation complete!"

# ============================================================================
# DEVELOPMENT TOOLS
# ============================================================================

# Format code using clang-format
format:
	@command -v clang-format >/dev/null 2>&1 || { \
		echo "clang-format not found! Install with:"; \
		echo "  Ubuntu/Debian: sudo apt install clang-format"; \
		echo "  macOS: brew install clang-format"; \
		exit 1; \
	}
	@echo "Formatting code..."
	clang-format -i $(SOURCES)
	@echo "Formatting complete!"

# Run static analysis
lint:
	@command -v clang-tidy >/dev/null 2>&1 || { \
		echo "clang-tidy not found!"; \
		exit 1; \
	}
	@echo "Running linter..."
	clang-tidy $(SOURCES) -- $(CXXFLAGS)

# Run various checks
check: lint
	@command -v cppcheck >/dev/null 2>&1 || { \
		echo "cppcheck not found!"; \
		exit 1; \
	}
	@echo "Running static analysis..."
	cppcheck --enable=all --suppress=missingIncludeSystem $(SOURCES)

# ============================================================================
# GIT OPERATIONS
# ============================================================================

push:
	@echo "Pushing to origin/main..."
	git push -u origin main
	@echo "Push complete!"

# Optional: Add a target that runs checks before pushing
push-safe: check-pre-push
	@echo "All checks passed, pushing to origin/main..."
	git push -u origin main
	@echo "Push complete!"

# Optional: Add commit and push in one command
commit-push:
	@echo "Enter commit message: "; \
	read MSG; \
	git add -A && git commit -m "$$MSG" && git push -u origin main

# ============================================================================
# TESTING TARGETS
# ============================================================================

# Basic functionality test
test: $(TARGET)
	@echo "Running basic tests..."
	./$(TARGET) --help
	./$(TARGET) --version 2>/dev/null || true
	./$(TARGET) /tmp
	@echo "Basic tests passed!"

# Test interactive mode
test-interactive: $(TARGET)
	@echo "Testing interactive mode..."
	@echo "Press 'q' to exit the interactive mode"
	./$(TARGET) i /tmp

# Test aggregate mode with various options
test-aggregate: $(TARGET)
	@echo "Testing aggregate mode..."
	./$(TARGET) a /tmp
	./$(TARGET) --tree /tmp
	./$(TARGET) --tree --top 5 /tmp
	./$(TARGET) --format binary /tmp
	./$(TARGET) --apparent-size /tmp

# Memory leak check (requires valgrind)
test-memory: debug
	@command -v valgrind >/dev/null 2>&1 || { \
		echo "valgrind not found! Install with:"; \
		echo "  Ubuntu/Debian: sudo apt install valgrind"; \
		echo "  macOS: brew install valgrind"; \
		exit 1; \
	}
	@echo "Running memory leak check..."
	valgrind --leak-check=full --show-leak-kinds=all \
		./$(TARGET)_debug a /tmp

# ============================================================================
# HELP TARGET
# ============================================================================

help:
	@echo "Enhanced DUA C++ Makefile"
	@echo ""
	@echo "Basic usage:"
	@echo "  make              - Build the standard version"
	@echo "  make clean        - Remove build artifacts (safe!)"
	@echo "  make help         - Show this help"
	@echo ""
	@echo "Build variants:"
	@echo "  make debug        - Build with debug symbols"
	@echo "  make release      - Build optimized version"
	@echo "  make static       - Build statically linked"
	@echo ""
	@echo "Installation:"
	@echo "  make install      - Install to system (PREFIX=$(PREFIX))"
	@echo "  make uninstall    - Remove from system"
	@echo ""
	@echo "Development:"
	@echo "  make format       - Format code with clang-format"
	@echo "  make lint         - Run clang-tidy"
	@echo "  make check        - Run static analysis"
	@echo ""
	@echo "Git targets:"
	@echo "  push              - Push to origin/main"
	@echo "  push-safe         - Run checks then push"
	@echo "  commit-push       - Add, commit, and push all changes"
	@echo ""
	@echo "Testing:"
	@echo "  make test         - Run basic tests"
	@echo "  make test-memory  - Check for memory leaks"
	@echo ""
	@echo "Options:"
	@echo "  DEBUG=1           - Enable debug build"
	@echo "  STATIC=1          - Enable static linking"
	@echo "  LTO=1             - Enable link-time optimization"
	@echo "  NATIVE=1          - Enable CPU-specific optimizations"
	@echo "  CXX=clang++       - Use different compiler"
	@echo "  PREFIX=/opt       - Change installation prefix"
	@echo ""
	@echo "Examples:"
	@echo "  make DEBUG=1                    # Debug build"
	@echo "  make STATIC=1 LTO=1             # Static optimized build"
	@echo "  make CXX=clang++ release        # Use clang++ for release"
	@echo "  make PREFIX=~/.local install    # Install to home directory"

# ============================================================================
# CONFIGURATION DISPLAY
# ============================================================================

show-config:
	@echo "Current configuration:"
	@echo "  CXX: $(CXX)"
	@echo "  CXXFLAGS: $(CXXFLAGS)"
	@echo "  LDFLAGS: $(LDFLAGS)"
	@echo "  Platform: $(UNAME_S) $(UNAME_M)"
	@echo "  Source files: $(SOURCES)"
	@echo "  Target: $(TARGET)"

# ============================================================================
# DEPENDENCY TRACKING
# ============================================================================

# Include auto-generated dependency files if they exist
# The - prefix means "don't error if these don't exist"
-include $(DEPS)

# Rule to generate dependency files
# This helps Make understand when headers change
%.d: %.cpp
	@$(CXX) $(CXXFLAGS) -MM -MT $(@:.d=.o) $< > $@

# ============================================================================
# END OF MAKEFILE
# ============================================================================
