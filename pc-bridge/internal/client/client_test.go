package client

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"sync/atomic"
	"testing"
	"time"
)

func TestNormalizeCodecAndStreamURL(t *testing.T) {
	cases := []struct {
		in      string
		want    string
		ok      bool
		urlPath string
	}{
		{"h264", "h264", true, "/stream.h264"},
		{"AVC", "h264", true, "/stream.h264"},
		{"h265", "h265", true, "/stream.h265"},
		{"HEVC", "h265", true, "/stream.h265"},
		{"av1", "av1", true, "/stream.av1"},
		{"AV01", "av1", true, "/stream.av1"},
		{"mjpeg", "mjpeg", true, "/stream.mjpg"},
		{"mjpg", "mjpeg", true, "/stream.mjpg"},
		{"vp9", "", false, "/stream.h264"},
	}
	for _, tc := range cases {
		got, ok := NormalizeCodec(tc.in)
		if got != tc.want || ok != tc.ok {
			t.Fatalf("NormalizeCodec(%q) = (%q, %v), want (%q, %v)", tc.in, got, ok, tc.want, tc.ok)
		}
		if gotURL := StreamURL("http://127.0.0.1:8080/", tc.in); gotURL != "http://127.0.0.1:8080"+tc.urlPath {
			t.Fatalf("StreamURL(%q) = %q, want %q", tc.in, gotURL, "http://127.0.0.1:8080"+tc.urlPath)
		}
	}
}

func TestGetStatusAndUpdateConfig(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/api/status":
			_ = json.NewEncoder(w).Encode(Status{
				Streaming:  true,
				CameraID:   "0",
				Width:      1920,
				Height:     1080,
				FPS:        30,
				Codec:      "h264",
				StreamPath: "/stream.h264",
				SupportedCodecs: []CodecInfo{
					{ID: "h264", Supported: true, Hardware: true},
					{ID: "h265", Supported: true, Hardware: true},
					{ID: "av1", Supported: false, Hardware: false},
					{ID: "mjpeg", Supported: true, Hardware: false},
				},
			})
		case "/api/config":
			var patch map[string]any
			if err := json.NewDecoder(r.Body).Decode(&patch); err != nil {
				http.Error(w, err.Error(), http.StatusBadRequest)
				return
			}
			_ = json.NewEncoder(w).Encode(Status{
				Streaming: true,
				CameraID:  "0",
				Width:     1280,
				Height:    720,
				FPS:       30,
				Codec:     patch["codec"].(string),
			})
		default:
			http.NotFound(w, r)
		}
	}))
	defer srv.Close()

	c := New()
	st, err := c.GetStatus(context.Background(), srv.URL)
	if err != nil {
		t.Fatalf("GetStatus error: %v", err)
	}
	if !st.Streaming || st.Width != 1920 || st.Codec != "h264" || len(st.SupportedCodecs) != 4 {
		t.Fatalf("unexpected status: %+v", st)
	}

	updated, err := c.UpdateConfig(context.Background(), srv.URL, map[string]any{"codec": "h265"})
	if err != nil {
		t.Fatalf("UpdateConfig error: %v", err)
	}
	if updated.Codec != "h265" {
		t.Fatalf("expected codec h265, got %q", updated.Codec)
	}
}

func TestWaitForReady(t *testing.T) {
	var attempts atomic.Int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		n := attempts.Add(1)
		_ = json.NewEncoder(w).Encode(Status{
			Streaming: n >= 2,
			Codec:     "h265",
		})
	}))
	defer srv.Close()

	c := New()
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()

	st, err := c.WaitForReady(ctx, srv.URL, 20*time.Millisecond)
	if err != nil {
		t.Fatalf("WaitForReady failed: %v", err)
	}
	if !st.Streaming || st.Codec != "h265" {
		t.Fatalf("unexpected status: %+v", st)
	}
}
