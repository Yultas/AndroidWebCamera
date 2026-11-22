package proxy

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os/exec"
	"testing"
	"time"

	"androidwebcamera/pc-bridge/internal/client"
	"androidwebcamera/pc-bridge/internal/v4l2"
)

func runFFmpegGen(t *testing.T, args ...string) []byte {
	t.Helper()
	cmd := exec.Command("ffmpeg", args...)
	var out, errBuf bytes.Buffer
	cmd.Stdout = &out
	cmd.Stderr = &errBuf
	if err := cmd.Run(); err != nil {
		t.Fatalf("ffmpeg %v failed (%v): %s", args, err, errBuf.String())
	}
	if out.Len() == 0 {
		t.Fatalf("ffmpeg %v produced empty output", args)
	}
	return out.Bytes()
}

func TestEndToEndClientAndFFmpegDecodeAllCodecs(t *testing.T) {
	ffmpegPath, err := v4l2.FindFFmpeg("")
	if err != nil {
		t.Skipf("ffmpeg unavailable in environment: %v", err)
	}

	h264Data := runFFmpegGen(t,
		"-hide_banner", "-loglevel", "error",
		"-f", "lavfi", "-i", "testsrc=size=320x240:rate=15",
		"-t", "0.5",
		"-c:v", "libx264", "-profile:v", "baseline", "-pix_fmt", "yuv420p",
		"-f", "h264", "pipe:1",
	)
	h265Data := runFFmpegGen(t,
		"-hide_banner", "-loglevel", "error",
		"-f", "lavfi", "-i", "testsrc=size=320x240:rate=15",
		"-t", "0.5",
		"-c:v", "libx265", "-pix_fmt", "yuv420p",
		"-f", "hevc", "pipe:1",
	)
	av1IvfData := runFFmpegGen(t,
		"-hide_banner", "-loglevel", "error",
		"-f", "lavfi", "-i", "testsrc=size=320x240:rate=15",
		"-t", "0.5",
		"-c:v", "libaom-av1", "-cpu-used", "8", "-usage", "realtime", "-pix_fmt", "yuv420p",
		"-f", "ivf", "pipe:1",
	)
	jpegFrame := runFFmpegGen(t,
		"-hide_banner", "-loglevel", "error",
		"-f", "lavfi", "-i", "testsrc=size=320x240:rate=15",
		"-frames:v", "1",
		"-f", "image2", "-vcodec", "mjpeg", "pipe:1",
	)

	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/api/status":
			w.Header().Set("Content-Type", "application/json")
			_ = json.NewEncoder(w).Encode(client.Status{
				Streaming:  true,
				CameraID:   "0",
				CameraName: "Основная",
				Width:      320,
				Height:     240,
				FPS:        15,
				Codec:      "h265",
				Rotation:   90,
			})
		case "/stream.h264":
			w.Header().Set("Content-Type", "video/h264")
			_, _ = w.Write(h264Data)
		case "/stream.h265":
			w.Header().Set("Content-Type", "video/hevc")
			_, _ = w.Write(h265Data)
		case "/stream.av1":
			w.Header().Set("Content-Type", "video/x-ivf")
			_, _ = w.Write(av1IvfData)
		case "/stream.mjpg":
			w.Header().Set("Content-Type", "multipart/x-mixed-replace; boundary=awcframe")
			for i := 0; i < 4; i++ {
				_, _ = fmt.Fprintf(w, "--awcframe\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n", len(jpegFrame))
				_, _ = w.Write(jpegFrame)
				_, _ = w.Write([]byte("\r\n"))
			}
		default:
			http.NotFound(w, r)
		}
	}))
	defer srv.Close()

	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()

	api := client.New()
	st, err := api.WaitForReady(ctx, srv.URL, 20*time.Millisecond)
	if err != nil {
		t.Fatalf("WaitForReady failed: %v", err)
	}

	rot, err := v4l2.ResolveRotation("auto", st.Rotation)
	if err != nil || rot != 90 {
		t.Fatalf("ResolveRotation = (%d, %v), want (90, nil)", rot, err)
	}

	codecs := []struct {
		name   string
		rot    int
		mirror bool
	}{
		{"h264", 90, true},
		{"h265", 180, true},
		{"av1", 270, false},
		{"mjpeg", 0, false},
	}

	for _, tc := range codecs {
		t.Run(tc.name, func(t *testing.T) {
			args := v4l2.BuildFFmpegArgsForOutput(v4l2.Options{
				StreamURL: client.StreamURL(srv.URL, tc.name),
				Codec:     tc.name,
				Rotation:  tc.rot,
				Mirror:    tc.mirror,
			}, "null", "-")
			cmd := exec.CommandContext(ctx, ffmpegPath, args...)
			if out, err := cmd.CombinedOutput(); err != nil {
				t.Fatalf("ffmpeg %s pipeline failed (%v): %s", tc.name, err, string(out))
			}
		})
	}
}
