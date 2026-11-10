# Linux build (WSL, Docker, Render). Windows build is still build.bat.
# Needs: g++ (C++20) and libmysqlclient-dev (provides mysql_config).
CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -pthread
MYSQL_CFLAGS := $(shell mysql_config --cflags)
MYSQL_LIBS   := $(shell mysql_config --libs)

server: src/*.cpp include/*.h
	$(CXX) $(CXXFLAGS) -Iinclude $(MYSQL_CFLAGS) -o $@ src/*.cpp $(MYSQL_LIBS)

clean:
	rm -f server

.PHONY: clean
