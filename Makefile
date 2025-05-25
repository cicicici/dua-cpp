# Makefile for dua-cli C++ implementation

CXX = g++
CXXFLAGS = -std=c++17 -O3 -Wall -Wextra -pthread
LDFLAGS = -lncurses -lpthread

# Debug flags
DEBUG_FLAGS = -g -O0 -DDEBUG

# Target binary name
TARGET = dua

# Source files
SOURCES = dua.cpp
OBJECTS = $(SOURCES:.cpp=.o)

# Default target
all: $(TARGET)

# Build the main binary
$(TARGET): $(OBJECTS)
	$(CXX) $(OBJECTS) -o $(TARGET) $(LDFLAGS)

# Compile source files
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Debug build
debug: CXXFLAGS += $(DEBUG_FLAGS)
debug: clean $(TARGET)

# Static build (for better portability)
static: LDFLAGS += -static
static: $(TARGET)

# Install target
PREFIX ?= /usr/local
install: $(TARGET)
	install -d $(PREFIX)/bin
	install -m 755 $(TARGET) $(PREFIX)/bin/

# Uninstall
uninstall:
	rm -f $(PREFIX)/bin/$(TARGET)

# Clean build artifacts
clean:
	rm -f $(TARGET) $(OBJECTS)

# Run tests
test: $(TARGET)
	./run_tests.sh

# Format code (requires clang-format)
format:
	clang-format -i $(SOURCES)

# Check for memory leaks (requires valgrind)
memcheck: debug
	valgrind --leak-check=full --show-leak-kinds=all ./$(TARGET)

# Performance profiling (requires perf)
profile: $(TARGET)
	perf record -g ./$(TARGET)
	perf report

.PHONY: all clean debug static install uninstall test format memcheck profile
