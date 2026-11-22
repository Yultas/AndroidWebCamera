package v4l2

import (
	"fmt"
	"os"
	"os/exec"
	"strconv"
	"strings"
)

// Options configures an FFmpeg decode -> virtual camera (or null sink) pipeline.
type Options struct {
	StreamURL  string
	Codec      string
	Device     string
	Rotation   int
	Mirror     bool
	FFmpegPath string
}

// FindFFmpeg locates the ffmpeg binary.
func FindFFmpeg(explicit string) (string, error) {
	if explicit != "" {
		if info, err := os.Stat(explicit); err == nil && !info.IsDir() {
			return explicit, nil
		}
		return "", fmt.Errorf("ffmpeg не найден по пути: %s", explicit)
	}
	path, err := exec.LookPath("ffmpeg")
	if err != nil {
		return "", fmt.Errorf("ffmpeg не найден в PATH (установите пакет ffmpeg)")
	}
	return path, nil
}

// ResolveRotation parses `--rotate` ("auto" or 0/90/180/270) using phone status rotation for "auto".
func ResolveRotation(flagValue string, statusRotation int) (int, error) {
	v := strings.TrimSpace(strings.ToLower(flagValue))
	if v == "" || v == "auto" {
		return ((statusRotation % 360) + 360) % 360, nil
	}
	deg, err := strconv.Atoi(v)
	if err != nil {
		return 0, fmt.Errorf("некорректное значение --rotate %q (допустимо: auto, 0, 90, 180, 270)", flagValue)
	}
	switch ((deg % 360) + 360) % 360 {
	case 0, 90, 180, 270:
		return ((deg % 360) + 360) % 360, nil
	default:
		return 0, fmt.Errorf("неподдерживаемый угол --rotate %d (допустимо: 0, 90, 180, 270)", deg)
	}
}

// BuildFFmpegArgs builds low-latency FFmpeg CLI flags targeting a v4l2loopback device.
func BuildFFmpegArgs(opts Options) []string {
	return BuildFFmpegArgsForOutput(opts, "v4l2", opts.Device)
}

// BuildFFmpegArgsForOutput builds FFmpeg flags targeting an arbitrary muxer/output (shared by v4l2 and tests).
func BuildFFmpegArgsForOutput(opts Options, outputFormat, outputTarget string) []string {
	args := []string{
		"-nostdin",
		"-hide_banner",
		"-loglevel", "warning",
	}

	codec := strings.ToLower(strings.TrimSpace(opts.Codec))
	switch codec {
	case "mjpeg", "mjpg":
		args = append(args,
			"-fflags", "nobuffer",
			"-flags", "low_delay",
		)
	case "av1", "av01":
		// Omit -fflags nobuffer for IVF/AV1 so libdav1d retains the initial Sequence Header OBU during probe.
		args = append(args,
			"-flags", "low_delay",
			"-analyzeduration", "500000",
			"-probesize", "262144",
		)
	default:
		// H.264 / H.265: 128KB probe balances fast startup on static scenes with high-bitrate 1080p/4K IDR frames.
		args = append(args,
			"-fflags", "nobuffer",
			"-flags", "low_delay",
			"-analyzeduration", "1000000",
			"-probesize", "131072",
		)
	}

	if strings.HasPrefix(opts.StreamURL, "http://") || strings.HasPrefix(opts.StreamURL, "https://") {
		args = append(args, "-rw_timeout", "5000000")
	}

	switch codec {
	case "mjpeg", "mjpg":
		args = append(args, "-f", "mpjpeg")
	case "h265", "hevc":
		args = append(args, "-f", "hevc")
	case "av1", "av01":
		args = append(args, "-f", "ivf")
	default:
		args = append(args, "-f", "h264")
	}

	args = append(args, "-i", opts.StreamURL)

	var filters []string
	switch ((opts.Rotation % 360) + 360) % 360 {
	case 90:
		filters = append(filters, "transpose=1")
	case 180:
		filters = append(filters, "transpose=2,transpose=2")
	case 270:
		filters = append(filters, "transpose=2")
	}
	if opts.Mirror {
		filters = append(filters, "hflip")
	}
	if len(filters) > 0 {
		args = append(args, "-vf", strings.Join(filters, ","))
	}

	if outputFormat == "v4l2" {
		args = append(args, "-pix_fmt", "yuyv422")
	}
	args = append(args, "-f", outputFormat, outputTarget)
	return args
}
