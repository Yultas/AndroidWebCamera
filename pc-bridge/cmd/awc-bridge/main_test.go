package main

import (
	"strings"
	"testing"
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
