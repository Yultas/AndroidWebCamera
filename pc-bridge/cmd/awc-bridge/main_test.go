package main

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"androidwebcamera/pc-bridge/internal/client"
)

func TestResolveMirror(t *testing.T) {
	tests := []struct {
		flag   string
		status bool
		want   bool
		err    bool
	}{
		{"auto", true, true, false},
		{"", false, false, false},
		{"true", false, true, false},
		{"false", true, false, false},
		{"invalid", false, false, true},
	}

	for _, tc := range tests {
		got, err := resolveMirror(tc.flag, tc.status)
		if (err != nil) != tc.err {
			t.Fatalf("resolveMirror(%q, %v) err=%v, wantErr=%v", tc.flag, tc.status, err, tc.err)
		}
		if !tc.err && got != tc.want {
			t.Fatalf("resolveMirror(%q, %v) = %v, want %v", tc.flag, tc.status, got, tc.want)
		}
	}
}

func TestRunFlagValidation(t *testing.T) {
	if err := run([]string{"--version"}); err != nil {
		t.Fatalf("--version failed: %v", err)
	}

	cases := []struct {
		args    []string
		wantSub string
	}{
		{[]string{"--mode", "bluetooth"}, "неизвестный --mode"},
		{[]string{"--mode", "wifi"}, "укажите адрес телефона"},
		{[]string{"--mode", "wifi", "--url", "http://127.0.0.1:9999", "--codec", "vp9"}, "неизвестный --codec"},
	}

	for _, tc := range cases {
		err := run(tc.args)
		if err == nil || !strings.Contains(err.Error(), tc.wantSub) {
			t.Fatalf("run(%v) = %v, expected substring %q", tc.args, err, tc.wantSub)
		}
	}
}

func TestEnsureCodecUnsupportedAndFallback(t *testing.T) {
	posts := 0
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/api/config" {
			posts++
			// Simulate Android auto-downgrading / keeping h264 instead of requested h265 at 4K60.
			_ = json.NewEncoder(w).Encode(client.Status{
				Streaming: true,
				Codec:     "h264",
			})
			return
		}
		http.NotFound(w, r)
	}))
	defer srv.Close()

	api := client.New()
	initial := client.Status{
		Streaming: true,
		Codec:     "h264",
		SupportedCodecs: []client.CodecInfo{
			{ID: "h264", Supported: true},
			{ID: "h265", Supported: true},
			{ID: "av1", Supported: false},
		},
	}

	// 1. Unsupported codec is rejected immediately without POSTing /api/config.
	reqAv1 := "av1"
	if _, err := ensureCodec(context.Background(), api, srv.URL, initial, &reqAv1); err == nil {
		t.Fatalf("expected error for unsupported codec av1")
	}
	if posts != 0 {
		t.Fatalf("expected 0 /api/config POSTs for unsupported codec, got %d", posts)
	}

	// 2. Auto-fallback clears requestedCodec so reconnects do not loop re-POSTing.
	reqH265 := "h265"
	st, err := ensureCodec(context.Background(), api, srv.URL, initial, &reqH265)
	if err != nil {
		t.Fatalf("unexpected error on fallback: %v", err)
	}
	if st.Codec != "h264" || reqH265 != "" {
		t.Fatalf("expected fallback to keep h264 and clear reqH265, got codec=%q req=%q", st.Codec, reqH265)
	}
	if posts != 1 {
		t.Fatalf("expected 1 POST, got %d", posts)
	}
}
