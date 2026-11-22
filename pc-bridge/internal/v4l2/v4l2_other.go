//go:build !linux

package v4l2

import (
	"context"
	"fmt"
	"io"
	"runtime"
)

// LoopbackDevice represents a virtual camera slot on Linux (unused on other OSes).
type LoopbackDevice struct {
	Path  string
	Label string
}

// ResolveDevice explains that v4l2loopback is Linux-only.
func ResolveDevice(spec string) (LoopbackDevice, error) {
	return LoopbackDevice{}, fmt.Errorf(
		"флаг --v4l2 (%s) поддерживается только на Linux (v4l2loopback). На %s используйте браузер или OBS Studio (Media Source + встроенная Virtual Camera)",
		spec,
		runtime.GOOS,
	)
}

// RunPipeline is unavailable on non-Linux hosts.
func RunPipeline(_ context.Context, _ string, _ func(context.Context) (Options, error), _ io.Writer) error {
	return fmt.Errorf(
		"виртуальная камера v4l2loopback доступна только на Linux; на %s передавайте поток в браузер или OBS Studio",
		runtime.GOOS,
	)
}
