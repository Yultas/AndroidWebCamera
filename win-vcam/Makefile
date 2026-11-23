CXX ?= g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wpedantic
FFMPEG_CFLAGS := $(shell pkg-config --cflags libavformat libavcodec libavutil libswscale)
FFMPEG_LIBS := $(shell pkg-config --libs libavformat libavcodec libavutil libswscale)

OBS_SRC := ../obs-plugin/src
BUILD_DIR := build-make

.PHONY: test host clean

test: $(BUILD_DIR)/test_vcam
	./$(BUILD_DIR)/test_vcam

host: $(BUILD_DIR)/awc-vcam

$(BUILD_DIR)/test_vcam: tests/test_vcam.cpp src/vcam-shm.cpp src/vcam-convert.cpp $(OBS_SRC)/awc-client.cpp $(OBS_SRC)/awc-decoder.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -Isrc -I$(OBS_SRC) $(FFMPEG_CFLAGS) $^ -o $@ $(FFMPEG_LIBS) -lpthread

$(BUILD_DIR)/awc-vcam: src/vcam-host.cpp src/vcam-shm.cpp $(OBS_SRC)/awc-adb.cpp $(OBS_SRC)/awc-client.cpp $(OBS_SRC)/awc-decoder.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -Isrc -I$(OBS_SRC) $(FFMPEG_CFLAGS) $^ -o $@ $(FFMPEG_LIBS) -lpthread

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)
