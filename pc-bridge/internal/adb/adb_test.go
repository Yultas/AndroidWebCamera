package adb

import (
	"context"
	"os"
	"path/filepath"
	"reflect"
	"runtime"
	"strings"
	"testing"
)

type fakeRunner struct {
	output string
	err    error
	calls  [][]string
}

func (f *fakeRunner) Run(_ context.Context, _ string, args ...string) (string, error) {
	f.calls = append(f.calls, args)
	return f.output, f.err
}

func TestParseDevicesOutput(t *testing.T) {
	raw := `* daemon not running; starting now at tcp:5037
* daemon started successfully
List of devices attached
R58N12345AB	device usb:1-2 product:a51 model:SM_A515F device:a51 transport_id:4
emulator-5554	unauthorized transport_id:2
`
	got := ParseDevicesOutput(raw)
	want := []Device{
		{Serial: "R58N12345AB", State: "device", Model: "SM_A515F"},
		{Serial: "emulator-5554", State: "unauthorized", Model: ""},
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("ParseDevicesOutput() = %+v, want %+v", got, want)
	}
}

func TestPickOnlineDeviceAndForward(t *testing.T) {
	runner := &fakeRunner{
		output: "List of devices attached\nABC123\tdevice model:Pixel_8\n",
	}
	mgr := &Manager{Bin: "adb", Runner: runner}

	dev, err := mgr.PickOnlineDevice(context.Background(), "")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if dev.Serial != "ABC123" || dev.Model != "Pixel_8" {
		t.Fatalf("unexpected device: %+v", dev)
	}

	if err := mgr.Forward(context.Background(), dev.Serial, 8080, 8080); err != nil {
		t.Fatalf("Forward failed: %v", err)
	}
	if err := mgr.RemoveForward(context.Background(), dev.Serial, 8080); err != nil {
		t.Fatalf("RemoveForward failed: %v", err)
	}

	if len(runner.calls) != 3 {
		t.Fatalf("expected 3 adb calls, got %d", len(runner.calls))
	}
	wantFwd := []string{"-s", "ABC123", "forward", "tcp:8080", "tcp:8080"}
	if !reflect.DeepEqual(runner.calls[1], wantFwd) {
		t.Fatalf("forward call = %v, want %v", runner.calls[1], wantFwd)
	}
}

func TestPickOnlineDeviceErrors(t *testing.T) {
	tests := []struct {
		name    string
		out     string
		serial  string
		wantSub string
	}{
		{
			name:    "no devices",
			out:     "List of devices attached\n\n",
			wantSub: "не найдено",
		},
		{
			name:    "unauthorized",
			out:     "List of devices attached\nDEV1\tunauthorized\n",
			wantSub: "не авторизовано",
		},
		{
			name:    "multiple without serial",
			out:     "List of devices attached\nDEV1\tdevice\nDEV2\tdevice\n",
			wantSub: "--serial",
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			mgr := &Manager{
				Bin:    "adb",
				Runner: &fakeRunner{output: tc.out},
			}
			_, err := mgr.PickOnlineDevice(context.Background(), tc.serial)
			if err == nil || !strings.Contains(err.Error(), tc.wantSub) {
				t.Fatalf("expected error containing %q, got %v", tc.wantSub, err)
			}
		})
	}
}

func TestFindAdbExplicitAndSdkFallback(t *testing.T) {
	tmp := t.TempDir()
	binName := "adb"
	if runtime.GOOS == "windows" {
		binName = "adb.exe"
	}
	sdkDir := filepath.Join(tmp, "Android", "Sdk")
	ptDir := filepath.Join(sdkDir, "platform-tools")
	if err := os.MkdirAll(ptDir, 0o755); err != nil {
		t.Fatal(err)
	}
	fakeAdb := filepath.Join(ptDir, binName)
	if err := os.WriteFile(fakeAdb, []byte("#!/bin/sh\n"), 0o755); err != nil {
		t.Fatal(err)
	}

	got, err := FindAdb(fakeAdb)
	if err != nil || got != fakeAdb {
		t.Fatalf("FindAdb(explicit) = (%q, %v), want %q", got, err, fakeAdb)
	}

	if _, err := FindAdb(filepath.Join(tmp, "missing-adb")); err == nil {
		t.Fatal("expected error for missing explicit adb path")
	}

	if runtime.GOOS != "windows" {
		t.Setenv("PATH", "")
		t.Setenv("HOME", tmp)
		t.Setenv("ANDROID_SDK_ROOT", sdkDir)
		gotSdk, err := FindAdb("")
		if err != nil || gotSdk != fakeAdb {
			t.Fatalf("FindAdb(sdk fallback) = (%q, %v), want %q", gotSdk, err, fakeAdb)
		}
	}
}
