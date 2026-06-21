# Define compilers
CC = gcc
CXX = g++

# Define compiler flags
CFLAGS = -Wall -Wextra -pedantic -std=c99
CXXFLAGS = -Wall -Wextra -pedantic -std=c++17

# Define the source files
CSRC = main.c
CXXSRC = main.cpp

# Define the target executables
CTARGET = main_c
CXXTARGET = main_cpp

# Build rule for the C executable
$(CTARGET): $(CSRC)
	$(CC) $(CFLAGS) $(CSRC) -o $(CTARGET)

# Build rule for the C++ executable
$(CXXTARGET): $(CXXSRC)
	$(CXX) $(CXXFLAGS) $(CXXSRC) -o $(CXXTARGET)

# Clean rule
clean:
	rm -f $(CTARGET) $(CXXTARGET)

# Count lines of code (requires cloc)
cloc:
	cloc $(CSRC)

# Generate test files and run benchmarks (Linux/WSL)
bench:
	$(MAKE) $(CTARGET)
	chmod +x scripts/generate_test_files.sh scripts/run_benchmarks.sh
	./scripts/run_benchmarks.sh
