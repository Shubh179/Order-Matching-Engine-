CXX = g++
CXXFLAGS = -O3 -march=native -flto -std=c++17 -Wall -Wextra -Wpedantic -pthread

TARGETS = server test_runner benchmark

all: $(TARGETS)

server: server.cpp order.h orderbook.h exchange.h spsc_queue.h latency_tracker.h
	$(CXX) $(CXXFLAGS) server.cpp -o server

test_runner: main.cpp order.h orderbook.h exchange.h
	$(CXX) $(CXXFLAGS) main.cpp -o test_runner

benchmark: benchmark.cpp order.h orderbook.h exchange.h spsc_queue.h mutex_queue.h latency_tracker.h
	$(CXX) $(CXXFLAGS) benchmark.cpp -o benchmark

clean:
	rm -f $(TARGETS)
.PHONY: all clean
