CXX ?= g++
PKG_CONFIG ?= pkg-config

CXXFLAGS_COMMON := -std=c++17 -O2 -Wall -Wextra -Wpedantic
FFMPEG_CFLAGS := $(shell $(PKG_CONFIG) --cflags libavformat libavcodec libavutil libswscale)
FFMPEG_LIBS := $(shell $(PKG_CONFIG) --libs libavformat libavcodec libavutil libswscale)

OBS_CFLAGS := $(shell if $(PKG_CONFIG) --exists libobs 2>/dev/null; then $(PKG_CONFIG) --cflags libobs; else echo "-Iinclude/obs-compat"; fi)

CORE_SRCS := src/awc-client.cpp src/awc-adb.cpp src/awc-decoder.cpp
PLUGIN_SRCS := $(CORE_SRCS) src/plugin-main.cpp
TEST_SRCS := $(PLUGIN_SRCS) tests/mock_obs.cpp tests/test_main.cpp

DIST_DIR := dist
BUILD_DIR := build
PLUGIN_SO := $(DIST_DIR)/obs-android-webcamera.so
TEST_BIN := $(BUILD_DIR)/awc-obs-tests

.PHONY: all build test install-user clean

all: build

build: $(PLUGIN_SO)
	@mkdir -p $(DIST_DIR)/data
	@cp -r data/* $(DIST_DIR)/data/

$(PLUGIN_SO): $(PLUGIN_SRCS) src/awc-client.hpp src/awc-adb.hpp src/awc-decoder.hpp
	@mkdir -p $(DIST_DIR)
	$(CXX) $(CXXFLAGS_COMMON) -fPIC -shared -fvisibility=hidden \
		$(OBS_CFLAGS) $(FFMPEG_CFLAGS) \
		$(PLUGIN_SRCS) -o $@ $(FFMPEG_LIBS) -lpthread

$(TEST_BIN): $(TEST_SRCS) tests/mock_obs.hpp src/awc-client.hpp src/awc-adb.hpp src/awc-decoder.hpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS_COMMON) -g -Iinclude/obs-compat -Isrc -Itests $(FFMPEG_CFLAGS) \
		$(TEST_SRCS) -o $@ $(FFMPEG_LIBS) -lpthread

test: $(TEST_BIN)
	./$(TEST_BIN)

install-user: build
	@PLUGIN_DIR="$${HOME}/.config/obs-studio/plugins/obs-android-webcamera"; \
	mkdir -p "$${PLUGIN_DIR}/bin/64bit" "$${PLUGIN_DIR}/data"; \
	cp "$(PLUGIN_SO)" "$${PLUGIN_DIR}/bin/64bit/"; \
	cp -r "$(DIST_DIR)/data/"* "$${PLUGIN_DIR}/data/"; \
	echo "Installed plugin to $${PLUGIN_DIR}"

clean:
	rm -rf $(BUILD_DIR) $(DIST_DIR)
