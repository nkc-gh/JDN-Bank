# JDN Bank - portable GNU Make build file
# The project deliberately avoids a hard dependency on CMake for classroom-friendly setup.

CXX := g++
# The service layer contains compact SQL statements; suppress only GCC's indentation warning.
# Other warnings and all compile errors remain enabled.
CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wno-misleading-indentation -O2 -Iinclude
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
LDLIBS := -Wl,-l:libsqlite3.so.0
else
LDLIBS := -lsqlite3
endif
TARGET := build/jdn-bank
SOURCES := $(wildcard src/*.cpp)

.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(SOURCES) | build
	$(CXX) $(CXXFLAGS) $(SOURCES) -o $@ $(LDLIBS)

build:
	mkdir -p build

run: all
	./$(TARGET)

clean:
	rm -rf build
