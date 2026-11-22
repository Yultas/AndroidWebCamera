//go:build linux

package v4l2

import (
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

// LoopbackDevice describes an available Linux v4l2loopback node.
type LoopbackDevice struct {
	Path  string
	Label string
}

// DetectLoopbackDevices discovers virtual cameras registered in sysfs by v4l2loopback.
func DetectLoopbackDevices() ([]LoopbackDevice, error) {
	entries, err := filepath.Glob("/sys/devices/virtual/video4linux/video*")
	if err != nil {
		return nil, err
	}
	sort.Strings(entries)

	var devices []LoopbackDevice
	for _, dir := range entries {
		base := filepath.Base(dir)
		devPath := filepath.Join("/dev", base)
		if _, err := os.Stat(devPath); err != nil {
			continue
		}
		nameBytes, _ := os.ReadFile(filepath.Join(dir, "name"))
		label := strings.TrimSpace(string(nameBytes))
		devices = append(devices, LoopbackDevice{
			Path:  devPath,
			Label: label,
		})
	}
	return devices, nil
}

// ResolveDevice validates an explicit /dev/videoX path or picks the first v4l2loopback device for "auto".
func ResolveDevice(spec string) (LoopbackDevice, error) {
	if strings.EqualFold(strings.TrimSpace(spec), "auto") {
		found, err := DetectLoopbackDevices()
		if err != nil {
			return LoopbackDevice{}, err
		}
		if len(found) == 0 {
			return LoopbackDevice{}, errors.New(
				"виртуальная камера v4l2loopback не найдена. Загрузите модуль ядра:\n" +
					"  sudo modprobe v4l2loopback exclusive_caps=1 card_label=\"AndroidWebCamera\"",
			)
		}
		for _, dev := range found {
			if strings.Contains(strings.ToLower(dev.Label), "androidwebcamera") {
				return dev, ValidateDevice(dev.Path)
			}
		}
		return found[0], ValidateDevice(found[0].Path)
	}

	if err := ValidateDevice(spec); err != nil {
		return LoopbackDevice{}, err
	}
	return LoopbackDevice{Path: spec}, nil
}

// ValidateDevice ensures the target /dev/videoX exists and can be opened for writing.
func ValidateDevice(path string) error {
	info, err := os.Stat(path)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return fmt.Errorf(
				"устройство %s не существует. Загрузите v4l2loopback:\n  sudo modprobe v4l2loopback exclusive_caps=1 card_label=\"AndroidWebCamera\"",
				path,
			)
		}
		return err
	}
	if info.Mode()&os.ModeCharDevice == 0 {
		return fmt.Errorf("%s не является символьным V4L2-устройством", path)
	}
	f, err := os.OpenFile(path, os.O_RDWR, 0)
	if err != nil {
		return fmt.Errorf("нет доступа на запись в %s (%w). Добавьте пользователя в группу video или проверьте права", path, err)
	}
	_ = f.Close()
	return nil
}

// RunPipeline runs ffmpeg in a self-healing loop so camera/resolution switches reconnect cleanly.
func RunPipeline(
	ctx context.Context,
	ffmpegBin string,
	resolveOpts func(context.Context) (Options, error),
	logOut io.Writer,
) error {
	for {
		if ctx.Err() != nil {
			return nil
		}

		opts, err := resolveOpts(ctx)
		if err != nil {
			if ctx.Err() != nil {
				return nil
			}
			fmt.Fprintf(logOut, "[v4l2] Ожидание потока: %v\n", err)
			select {
			case <-ctx.Done():
				return nil
			case <-time.After(1500 * time.Millisecond):
				continue
			}
		}

		args := BuildFFmpegArgs(opts)
		fmt.Fprintf(logOut, "[v4l2] Трансляция %s (%s) -> %s\n", opts.StreamURL, strings.ToUpper(opts.Codec), opts.Device)

		cmd := exec.CommandContext(ctx, ffmpegBin, args...)
		cmd.Stdout = logOut
		cmd.Stderr = logOut

		runErr := cmd.Run()
		if ctx.Err() != nil {
			return nil
		}
		if runErr != nil {
			fmt.Fprintf(logOut, "[v4l2] FFmpeg завершился (%v), переподключение через 1 с...\n", runErr)
		} else {
			fmt.Fprintf(logOut, "[v4l2] Поток перезапущен, переподключение...\n")
		}

		select {
		case <-ctx.Done():
			return nil
		case <-time.After(1 * time.Second):
		}
	}
}
