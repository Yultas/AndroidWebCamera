package v4l2

import (
	"reflect"
	"testing"
)

func TestResolveRotation(t *testing.T) {
	tests := []struct {
		flag    string
		status  int
		want    int
		wantErr bool
	}{
		{"auto", 90, 90, false},
		{"", 270, 270, false},
		{"0", 90, 0, false},
		{"180", 0, 180, false},
		{"45", 0, 0, true},
		{"bad", 0, 0, true},
	}

	for _, tc := range tests {
		got, err := ResolveRotation(tc.flag, tc.status)
		if (err != nil) != tc.wantErr {
			t.Fatalf("ResolveRotation(%q, %d) err=%v, wantErr=%v", tc.flag, tc.status, err, tc.wantErr)
		}
		if !tc.wantErr && got != tc.want {
			t.Fatalf("ResolveRotation(%q, %d) = %d, want %d", tc.flag, tc.status, got, tc.want)
		}
	}
}

func TestBuildFFmpegArgsAllCodecs(t *testing.T) {
	gotH264 := BuildFFmpegArgs(Options{
		StreamURL: "http://127.0.0.1:8080/stream.h264",
		Codec:     "h264",
		Device:    "/dev/video4",
		Rotation:  90,
		Mirror:    true,
	})
	wantH264 := []string{
		"-nostdin", "-hide_banner", "-loglevel", "warning",
		"-fflags", "nobuffer", "-flags", "low_delay",
		"-analyzeduration", "1000000", "-probesize", "131072",
		"-rw_timeout", "5000000",
		"-f", "h264", "-i", "http://127.0.0.1:8080/stream.h264",
		"-vf", "transpose=1,hflip",
		"-pix_fmt", "yuyv422",
		"-f", "v4l2", "/dev/video4",
	}
	if !reflect.DeepEqual(gotH264, wantH264) {
		t.Fatalf("BuildFFmpegArgs(h264) =\n  %v\nwant:\n  %v", gotH264, wantH264)
	}

	gotH265 := BuildFFmpegArgs(Options{
		StreamURL: "http://127.0.0.1:8080/stream.h265",
		Codec:     "h265",
		Device:    "/dev/video4",
		Rotation:  270,
		Mirror:    false,
	})
	wantH265 := []string{
		"-nostdin", "-hide_banner", "-loglevel", "warning",
		"-fflags", "nobuffer", "-flags", "low_delay",
		"-analyzeduration", "1000000", "-probesize", "131072",
		"-rw_timeout", "5000000",
		"-f", "hevc", "-i", "http://127.0.0.1:8080/stream.h265",
		"-vf", "transpose=2",
		"-pix_fmt", "yuyv422",
		"-f", "v4l2", "/dev/video4",
	}
	if !reflect.DeepEqual(gotH265, wantH265) {
		t.Fatalf("BuildFFmpegArgs(h265) =\n  %v\nwant:\n  %v", gotH265, wantH265)
	}

	gotAv1 := BuildFFmpegArgs(Options{
		StreamURL: "http://127.0.0.1:8080/stream.av1",
		Codec:     "av1",
		Device:    "/dev/video4",
		Rotation:  0,
		Mirror:    true,
	})
	wantAv1 := []string{
		"-nostdin", "-hide_banner", "-loglevel", "warning",
		"-flags", "low_delay",
		"-analyzeduration", "500000", "-probesize", "262144",
		"-rw_timeout", "5000000",
		"-f", "ivf", "-i", "http://127.0.0.1:8080/stream.av1",
		"-vf", "hflip",
		"-pix_fmt", "yuyv422",
		"-f", "v4l2", "/dev/video4",
	}
	if !reflect.DeepEqual(gotAv1, wantAv1) {
		t.Fatalf("BuildFFmpegArgs(av1) =\n  %v\nwant:\n  %v", gotAv1, wantAv1)
	}

	gotMjpeg := BuildFFmpegArgs(Options{
		StreamURL: "http://127.0.0.1:8080/stream.mjpg",
		Codec:     "mjpeg",
		Device:    "/dev/video4",
		Rotation:  180,
		Mirror:    false,
	})
	wantMjpeg := []string{
		"-nostdin", "-hide_banner", "-loglevel", "warning",
		"-fflags", "nobuffer", "-flags", "low_delay",
		"-rw_timeout", "5000000",
		"-f", "mpjpeg", "-i", "http://127.0.0.1:8080/stream.mjpg",
		"-vf", "transpose=2,transpose=2",
		"-pix_fmt", "yuyv422",
		"-f", "v4l2", "/dev/video4",
	}
	if !reflect.DeepEqual(gotMjpeg, wantMjpeg) {
		t.Fatalf("BuildFFmpegArgs(mjpeg) =\n  %v\nwant:\n  %v", gotMjpeg, wantMjpeg)
	}
}
