# Alternative a CMake : make && make test
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
HEADERS  := $(wildcard src/*.hpp)

all: build/news_server build/news_client build/unit_tests

build/news_server: src/server.cpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) -o $@ $<

build/news_client: src/client.cpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) -o $@ $<

build/unit_tests: tests/unit_tests.cpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) -o $@ $<

build:
	mkdir -p build

test: all
	./build/unit_tests
	./demo.sh build

clean:
	rm -rf build

.PHONY: all test clean
