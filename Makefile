# Makefile for Enhanced DUA C++ Implementation
# Supports multiple platforms and build configurations

# Compiler settings
CXX ?= g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread
LDFLAGS = -lpthread

# Platform detection
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

# Platform-specific settings
ifeq ($(UNAME_S),Linux)
    CXXFLAGS += -D__linux__
    LDFLAGS += -lncurses
    # Check for musl libc for fully static builds
    LIBC := $(shell ldd --version 2>&1 | grep -q musl && echo musl || echo glibc)
endif

ifeq ($(UNAME_S),Darwin)
    CXXFLAGS += -D__APPLE__
    # macOS ncurses is often in a different location
    CXXFLAGS += -I/usr/local/opt/ncurses/include
    LDFLAGS += -L/usr/local/opt/ncurses/lib -lncurses
endif

ifeq ($(UNAME_S),FreeBSD)
    CXXFLAGS += -D__FreeBSD__
    LDFLAGS += -lncurses
endif

# Build configurations
DEBUG ?= 0
STATIC ?= 0
LTO ?= 0
NATIVE ?= 0

# Apply build configurations
ifeq ($(DEBUG),1)
    CXXFLAGS += -g -O0 -DDEBUG
    TARGET_SUFFIX := _debug
else
    CXXFLAGS += -O3 -DNDEBUG
    TARGET_SUFFIX :=
endif

ifeq ($(STATIC),1)
    LDFLAGS += -static
    TARGET_SUFFIX := $(TARGET_SUFFIX)_static
endif

ifeq ($(LTO),1)
    CXXFLAGS += -flto
    LDFLAGS += -flto
endif

ifeq ($(NATIVE),1)
    CXXFLAGS += -march=native -mtune=native
endif

# Target binary names
TARGET_BASE = dua
TARGET = $(TARGET_BASE)$(TARGET_SUFFIX)

# Source files
SOURCES = dua_enhanced.cpp
OBJECTS = $(SOURCES:.cpp=.o)

# Installation directories
PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin
MANDIR = $(PREFIX)/share/man/man1

# Version information
VERSION = 2.30.1-cpp
GIT_HASH = $(shell git rev-parse --short HEAD 2>/dev/null || echo "unknown")
BUILD_DATE = $(shell date +%Y-%m-%d)

# Add version information to build
CXXFLAGS += -DDUA_VERSION=\"$(VERSION)\" -DGIT_HASH=\"$(GIT_HASH)\" -DBUILD_DATE=\"$(BUILD_DATE)\"

# Default target
.PHONY: all
all: $(TARGET)

# Main build target
$(TARGET): $(OBJECTS)
	@echo "Linking $@..."
	$(CXX) $(OBJECTS) -o $@ $(LDFLAGS)
	@echo "Build complete: $@"
	@echo "  Version: $(VERSION)"
	@echo "  Platform: $(UNAME_S) $(UNAME_M)"
	@echo "  Debug: $(DEBUG)"
	@echo "  Static: $(STATIC)"

# Compile source files
%.o: %.cpp
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Development builds
.PHONY: debug
debug:
	@$(MAKE) DEBUG=1

.PHONY: release
release:
	@$(MAKE) DEBUG=0 LTO=1 NATIVE=1

.PHONY: static
static:
	@$(MAKE) STATIC=1 LTO=1

# Platform-specific builds
.PHONY: linux-static
linux-static:
	@echo "Building fully static Linux binary..."
ifeq ($(LIBC),musl)
	$(CXX) $(CXXFLAGS) -O3 -static dua_enhanced.cpp -o dua_linux_static -lncurses -ltinfo
else
	@echo "Warning: glibc detected. Fully static builds work best with musl libc."
	$(CXX) $(CXXFLAGS) -O3 -static dua_enhanced.cpp -o dua_linux_static -lncurses -ltinfo -lgpm
endif

.PHONY: macos-universal
macos-universal:
	@echo "Building universal macOS binary..."
	$(CXX) $(CXXFLAGS) -O3 -arch x86_64 -arch arm64 dua_enhanced.cpp -o dua_macos_universal $(LDFLAGS)

# Installation targets
.PHONY: install
install: $(TARGET)
	@echo "Installing to $(BINDIR)..."
	install -d $(BINDIR)
	install -m 755 $(TARGET) $(BINDIR)/$(TARGET_BASE)
	@echo "Installation complete!"

.PHONY: uninstall
uninstall:
	@echo "Removing $(BINDIR)/$(TARGET_BASE)..."
	rm -f $(BINDIR)/$(TARGET_BASE)
	@echo "Uninstallation complete!"

# Development helpers
.PHONY: format
format:
	@echo "Formatting code..."
	@command -v clang-format >/dev/null 2>&1 || { echo "clang-format not found!"; exit 1; }
	clang-format -i $(SOURCES)

.PHONY: lint
lint:
	@echo "Running linter..."
	@command -v clang-tidy >/dev/null 2>&1 || { echo "clang-tidy not found!"; exit 1; }
	clang-tidy $(SOURCES) -- $(CXXFLAGS)

.PHONY: check
check: lint
	@echo "Running static analysis..."
	@command -v cppcheck >/dev/null 2>&1 || { echo "cppcheck not found!"; exit 1; }
	cppcheck --enable=all --suppress=missingIncludeSystem $(SOURCES)

# Testing targets
.PHONY: test
test: $(TARGET)
	@echo "Running basic tests..."
	./$(TARGET) --help
	./$(TARGET) /tmp
	@echo "Basic tests passed!"

.PHONY: test-interactive
test-interactive: $(TARGET)
	@echo "Testing interactive mode..."
	./$(TARGET) i /tmp /var/log

.PHONY: test-aggregate
test-aggregate: $(TARGET)
	@echo "Testing aggregate mode..."
	./$(TARGET) a --format binary /usr/bin
	./$(TARGET) a --apparent-size /tmp
	./$(TARGET) a --top 10 /var

.PHONY: test-memory
test-memory: debug
	@echo "Running memory leak check..."
	@command -v valgrind >/dev/null 2>&1 || { echo "valgrind not found!"; exit 1; }
	valgrind --leak-check=full --show-leak-kinds=all \
		./$(TARGET)_debug a /tmp

.PHONY: test-performance
test-performance: release
	@echo "Running performance test..."
	@command -v hyperfine >/dev/null 2>&1 || { echo "hyperfine not found!"; exit 1; }
	hyperfine --warmup 3 './$(TARGET) a /usr' 'du -sh /usr' 'ncdu -o- /usr'

# Benchmark against original dua
.PHONY: benchmark
benchmark: release
	@echo "Benchmarking against original dua (if available)..."
	@if command -v dua >/dev/null 2>&1; then \
		echo "Comparing with original dua..."; \
		hyperfine --warmup 2 \
			'./$(TARGET) a /usr/share' \
			'dua /usr/share'; \
	else \
		echo "Original dua not found, running standalone benchmark..."; \
		time ./$(TARGET) a /usr/share; \
	fi

# Distribution packages
.PHONY: dist
dist: clean
	@echo "Creating distribution archive..."
	mkdir -p dua-cpp-$(VERSION)
	cp -r *.cpp *.md Makefile LICENSE README dua-cpp-$(VERSION)/
	tar czf dua-cpp-$(VERSION).tar.gz dua-cpp-$(VERSION)
	rm -rf dua-cpp-$(VERSION)
	@echo "Created dua-cpp-$(VERSION).tar.gz"

.PHONY: deb
deb: static
	@echo "Creating Debian package..."
	# This is a simplified version - real debian packaging is more complex
	mkdir -p debian/usr/bin
	cp $(TARGET)_static debian/usr/bin/dua
	# Would need proper debian control files here
	@echo "Debian packaging not fully implemented"

.PHONY: rpm
rpm: static
	@echo "Creating RPM package..."
	# This is a simplified version - real RPM packaging is more complex
	@echo "RPM packaging not fully implemented"

# Clean targets
.PHONY: clean
clean:
	@echo "Cleaning build artifacts..."
	rm -f $(OBJECTS) $(TARGET_BASE) $(TARGET_BASE)_*
	rm -f *.o *.d core *.core
	@echo "Clean complete!"

.PHONY: distclean
distclean: clean
	@echo "Removing all generated files..."
	rm -f tags TAGS
	rm -f *.tar.gz *.zip
	rm -rf debian

# Help target
.PHONY: help
help:
	@echo "Enhanced DUA C++ Makefile"
	@echo ""
	@echo "Usage: make [target] [options]"
	@echo ""
	@echo "Main targets:"
	@echo "  all             - Build dua (default)"
	@echo "  debug           - Build with debug symbols"
	@echo "  release         - Build optimized release version"
	@echo "  static          - Build statically linked version"
	@echo "  install         - Install to system (PREFIX=$(PREFIX))"
	@echo "  uninstall       - Remove from system"
	@echo "  clean           - Remove build artifacts"
	@echo ""
	@echo "Platform targets:"
	@echo "  linux-static    - Build fully static Linux binary"
	@echo "  macos-universal - Build universal macOS binary"
	@echo ""
	@echo "Development targets:"
	@echo "  format          - Format code with clang-format"
	@echo "  lint            - Run clang-tidy linter"
	@echo "  check           - Run static analysis"
	@echo ""
	@echo "Testing targets:"
	@echo "  test            - Run basic tests"
	@echo "  test-memory     - Check for memory leaks"
	@echo "  test-performance - Run performance tests"
	@echo "  benchmark       - Compare with original dua"
	@echo ""
	@echo "Options:"
	@echo "  DEBUG=1         - Enable debug build"
	@echo "  STATIC=1        - Enable static linking"
	@echo "  LTO=1           - Enable link-time optimization"
	@echo "  NATIVE=1        - Enable native CPU optimizations"
	@echo "  PREFIX=/path    - Set installation prefix"
	@echo ""
	@echo "Examples:"
	@echo "  make                    # Build standard version"
	@echo "  make DEBUG=1            # Build debug version"
	@echo "  make release            # Build optimized version"
	@echo "  make static install     # Build and install static version"
	@echo "  make PREFIX=/opt/local install  # Install to /opt/local"

# Include dependency files if they exist
-include $(OBJECTS:.o=.d)

# Generate dependency files automatically
%.d: %.cpp
	@$(CXX) $(CXXFLAGS) -MM -MT $(@:.d=.o) $< > $@

# Ensure we have necessary tools for certain targets
.PHONY: check-tools
check-tools:
	@echo "Checking for required tools..."
	@command -v $(CXX) >/dev/null 2>&1 || { echo "$(CXX) not found!"; exit 1; }
	@command -v pkg-config >/dev/null 2>&1 || echo "Warning: pkg-config not found"
	@echo "Basic tools OK"

# CI/CD targets
.PHONY: ci
ci: clean check test
	@echo "CI checks passed!"

.PHONY: cd
cd: release test-performance dist
	@echo "CD build complete!"

# Show configuration
.PHONY: show-config
show-config:
	@echo "Build Configuration:"
	@echo "  CXX: $(CXX)"
	@echo "  CXXFLAGS: $(CXXFLAGS)"
	@echo "  LDFLAGS: $(LDFLAGS)"
	@echo "  Platform: $(UNAME_S) $(UNAME_M)"
	@echo "  Version: $(VERSION)"
	@echo "  Git Hash: $(GIT_HASH)"

# Performance profiling
.PHONY: profile
profile: debug
	@echo "Building for profiling..."
	$(CXX) $(CXXFLAGS) -pg dua_enhanced.cpp -o $(TARGET)_profile $(LDFLAGS)
	@echo "Run the program and then use 'gprof $(TARGET)_profile gmon.out'"

# Address sanitizer build
.PHONY: asan
asan:
	@echo "Building with AddressSanitizer..."
	$(CXX) $(CXXFLAGS) -fsanitize=address -fno-omit-frame-pointer \
		dua_enhanced.cpp -o $(TARGET)_asan $(LDFLAGS) -lasan

# Thread sanitizer build
.PHONY: tsan
tsan:
	@echo "Building with ThreadSanitizer..."
	$(CXX) $(CXXFLAGS) -fsanitize=thread -fno-omit-frame-pointer \
		dua_enhanced.cpp -o $(TARGET)_tsan $(LDFLAGS) -ltsan
