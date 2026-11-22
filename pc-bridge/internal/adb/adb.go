package adb

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
)

// Device represents an Android target discovered via `adb devices -l`.
type Device struct {
	Serial string
	State  string
	Model  string
}

// Runner abstracts command execution for unit testing without an actual adb binary.
type Runner interface {
	Run(ctx context.Context, bin string, args ...string) (string, error)
}

// ExecRunner invokes OS subprocesses with context cancellation.
type ExecRunner struct{}

func (ExecRunner) Run(ctx context.Context, bin string, args ...string) (string, error) {
	cmd := exec.CommandContext(ctx, bin, args...)
	var stdout, stderr bytes.Buffer
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	if err := cmd.Run(); err != nil {
		msg := strings.TrimSpace(stderr.String())
		if msg == "" {
			msg = err.Error()
		}
		return stdout.String(), fmt.Errorf("%s %s: %s", filepath.Base(bin), strings.Join(args, " "), msg)
	}
	return stdout.String(), nil
}

// Manager wraps adb CLI invocations.
type Manager struct {
	Bin    string
	Runner Runner
}

// NewManager constructs a Manager using the standard OS runner.
func NewManager(bin string) *Manager {
	return &Manager{
		Bin:    bin,
		Runner: ExecRunner{},
	}
}

// FindAdb locates the adb executable from explicit path, PATH, or default Android SDK locations.
func FindAdb(explicit string) (string, error) {
	if explicit != "" {
		if info, err := os.Stat(explicit); err == nil && !info.IsDir() {
			return explicit, nil
		}
		return "", fmt.Errorf("adb не найден по указанному пути: %s", explicit)
	}

	if path, err := exec.LookPath("adb"); err == nil {
		return path, nil
	}

	home, _ := os.UserHomeDir()
	var candidates []string
	if runtime.GOOS == "windows" {
		localAppData := os.Getenv("LOCALAPPDATA")
		if localAppData != "" {
			candidates = append(candidates, filepath.Join(localAppData, "Android", "Sdk", "platform-tools", "adb.exe"))
		}
		if home != "" {
			candidates = append(candidates, filepath.Join(home, "AppData", "Local", "Android", "Sdk", "platform-tools", "adb.exe"))
		}
	} else {
		if sdk := os.Getenv("ANDROID_SDK_ROOT"); sdk != "" {
			candidates = append(candidates, filepath.Join(sdk, "platform-tools", "adb"))
		}
		if sdk := os.Getenv("ANDROID_HOME"); sdk != "" {
			candidates = append(candidates, filepath.Join(sdk, "platform-tools", "adb"))
		}
		if home != "" {
			candidates = append(candidates,
				filepath.Join(home, "Android", "Sdk", "platform-tools", "adb"),
				"/usr/bin/adb",
				"/usr/local/bin/adb",
			)
		}
	}

	for _, c := range candidates {
		if info, err := os.Stat(c); err == nil && !info.IsDir() {
			return c, nil
		}
	}

	return "", errors.New("утилита adb не найдена в PATH (установите android-tools / Android Platform-Tools или передайте --adb)")
}

// ListDevices runs `adb devices -l` and parses all connected devices.
func (m *Manager) ListDevices(ctx context.Context) ([]Device, error) {
	out, err := m.Runner.Run(ctx, m.Bin, "devices", "-l")
	if err != nil {
		return nil, err
	}
	return ParseDevicesOutput(out), nil
}

// ParseDevicesOutput parses stdout from `adb devices -l`.
func ParseDevicesOutput(output string) []Device {
	var devices []Device
	for _, rawLine := range strings.Split(output, "\n") {
		line := strings.TrimSpace(rawLine)
		if line == "" ||
			strings.HasPrefix(line, "List of devices") ||
			strings.HasPrefix(line, "*") {
			continue
		}
		fields := strings.Fields(line)
		if len(fields) < 2 {
			continue
		}
		dev := Device{
			Serial: fields[0],
			State:  fields[1],
		}
		for _, field := range fields[2:] {
			if strings.HasPrefix(field, "model:") {
				dev.Model = strings.TrimPrefix(field, "model:")
				break
			}
		}
		devices = append(devices, dev)
	}
	return devices
}

// PickOnlineDevice selects an authorized online Android device or returns an actionable error.
func (m *Manager) PickOnlineDevice(ctx context.Context, preferredSerial string) (Device, error) {
	devices, err := m.ListDevices(ctx)
	if err != nil {
		return Device{}, err
	}
	if len(devices) == 0 {
		return Device{}, errors.New("Android-устройство не найдено по USB (проверьте кабель и включите Отладку по USB)")
	}

	if preferredSerial != "" {
		for _, d := range devices {
			if d.Serial == preferredSerial {
				if d.State != "device" {
					return Device{}, fmt.Errorf("устройство %s имеет статус '%s' (подтвердите отладку по USB на телефоне)", d.Serial, d.State)
				}
				return d, nil
			}
		}
		return Device{}, fmt.Errorf("устройство с serial '%s' не найдено", preferredSerial)
	}

	var online []Device
	for _, d := range devices {
		if d.State == "device" {
			online = append(online, d)
		}
	}
	if len(online) == 0 {
		return Device{}, fmt.Errorf("устройство найдено (%s), но не авторизовано (статус: %s). Разблокируйте телефон и разрешите отладку по USB", devices[0].Serial, devices[0].State)
	}
	if len(online) > 1 {
		var serials []string
		for _, d := range online {
			serials = append(serials, d.Serial)
		}
		return Device{}, fmt.Errorf("подключено несколько устройств (%s); укажите нужное через --serial", strings.Join(serials, ", "))
	}
	return online[0], nil
}

// Forward sets up `adb -s <serial> forward tcp:<localPort> tcp:<remotePort>`.
func (m *Manager) Forward(ctx context.Context, serial string, localPort, remotePort int) error {
	args := []string{}
	if serial != "" {
		args = append(args, "-s", serial)
	}
	args = append(args, "forward", fmt.Sprintf("tcp:%d", localPort), fmt.Sprintf("tcp:%d", remotePort))
	_, err := m.Runner.Run(ctx, m.Bin, args...)
	return err
}

// RemoveForward tears down a forwarded local TCP port.
func (m *Manager) RemoveForward(ctx context.Context, serial string, localPort int) error {
	args := []string{}
	if serial != "" {
		args = append(args, "-s", serial)
	}
	args = append(args, "forward", "--remove", fmt.Sprintf("tcp:%d", localPort))
	_, err := m.Runner.Run(ctx, m.Bin, args...)
	return err
}
