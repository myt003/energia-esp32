# Host-side unit tests. No PlatformIO, Arduino or ESP32 hardware required.
CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Werror -pedantic

TARGET := build/test_energia_ui_model
SOURCES := test_energia_ui_model.cpp ../src/energia_ui_model.cpp
HEADERS := ../src/energia_ui_model.h stubs/energia_test_stubs.h

.PHONY: all test clean

all: test

test: $(TARGET)
	./$(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(SOURCES) -o $(TARGET)

clean:
	rm -rf build
