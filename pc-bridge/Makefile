.PHONY: all test build-linux build-windows build-all clean

BINARY_NAME := awc-bridge
DIST_DIR := dist

all: test build-all

test:
	go vet ./...
	go test -v -race ./...

build-linux:
	mkdir -p $(DIST_DIR)
	CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -ldflags="-s -w" -o $(DIST_DIR)/$(BINARY_NAME)-linux-amd64 ./cmd/awc-bridge

build-windows:
	mkdir -p $(DIST_DIR)
	CGO_ENABLED=0 GOOS=windows GOARCH=amd64 go build -ldflags="-s -w" -o $(DIST_DIR)/$(BINARY_NAME)-windows-amd64.exe ./cmd/awc-bridge

build-all: build-linux build-windows

clean:
	rm -rf $(DIST_DIR)
