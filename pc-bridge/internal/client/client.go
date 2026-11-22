package client

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"
)

// CodecInfo describes device encoder support for a video codec reported in /api/status.
type CodecInfo struct {
	ID         string `json:"id"`
	Name       string `json:"name"`
	Supported  bool   `json:"supported"`
	Hardware   bool   `json:"hardware"`
	StreamPath string `json:"streamPath"`
	Encoder    string `json:"encoder"`
}

// Status mirrors the Android /api/status JSON payload.
type Status struct {
	Streaming       bool        `json:"streaming"`
	CameraID        string      `json:"cameraId"`
	CameraName      string      `json:"cameraName"`
	Width           int         `json:"width"`
	Height          int         `json:"height"`
	FPS             int         `json:"fps"`
	MeasuredFPS     int         `json:"measuredFps"`
	Codec           string      `json:"codec"`
	StreamPath      string      `json:"streamPath"`
	BitrateKbps     int         `json:"bitrateKbps"`
	JPEGQuality     int         `json:"jpegQuality"`
	Port            int         `json:"port"`
	Rotation        int         `json:"rotation"`
	Mirrored        bool        `json:"mirrored"`
	TorchEnabled    bool        `json:"torchEnabled"`
	HasFlash        bool        `json:"hasFlash"`
	Clients         int         `json:"clients"`
	SupportedCodecs []CodecInfo `json:"supportedCodecs,omitempty"`
}

// Client communicates with the AndroidWebCamera HTTP API.
type Client struct {
	http *http.Client
}

// New creates a Client with sensible request timeouts.
func New() *Client {
	return &Client{
		http: &http.Client{Timeout: 3 * time.Second},
	}
}

// NormalizeCodec canonicalizes user/API codec names (h264, h265/hevc, av1/av01, mjpeg/mjpg).
func NormalizeCodec(codec string) (string, bool) {
	switch strings.ToLower(strings.TrimSpace(codec)) {
	case "h264", "avc":
		return "h264", true
	case "h265", "hevc":
		return "h265", true
	case "av1", "av01":
		return "av1", true
	case "mjpeg", "mjpg":
		return "mjpeg", true
	default:
		return "", false
	}
}

// GetStatus fetches /api/status from baseURL.
func (c *Client) GetStatus(ctx context.Context, baseURL string) (Status, error) {
	url := strings.TrimRight(baseURL, "/") + "/api/status"
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return Status{}, err
	}
	resp, err := c.http.Do(req)
	if err != nil {
		return Status{}, err
	}
	defer resp.Body.Close()

	body, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil {
		return Status{}, err
	}
	if resp.StatusCode != http.StatusOK {
		return Status{}, fmt.Errorf("HTTP %d: %s", resp.StatusCode, strings.TrimSpace(string(body)))
	}

	var st Status
	if err := json.Unmarshal(body, &st); err != nil {
		return Status{}, fmt.Errorf("некорректный JSON от сервера: %w", err)
	}
	return st, nil
}

// UpdateConfig posts partial configuration updates to /api/config.
func (c *Client) UpdateConfig(ctx context.Context, baseURL string, patch map[string]any) (Status, error) {
	raw, err := json.Marshal(patch)
	if err != nil {
		return Status{}, err
	}
	url := strings.TrimRight(baseURL, "/") + "/api/config"
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, url, bytes.NewReader(raw))
	if err != nil {
		return Status{}, err
	}
	req.Header.Set("Content-Type", "application/json")
	resp, err := c.http.Do(req)
	if err != nil {
		return Status{}, err
	}
	defer resp.Body.Close()

	body, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil {
		return Status{}, err
	}
	if resp.StatusCode != http.StatusOK {
		return Status{}, fmt.Errorf("HTTP %d: %s", resp.StatusCode, strings.TrimSpace(string(body)))
	}

	var st Status
	if err := json.Unmarshal(body, &st); err != nil {
		return Status{}, err
	}
	return st, nil
}

// WaitForReady polls /api/status until the Android server responds and is actively streaming.
func (c *Client) WaitForReady(ctx context.Context, baseURL string, interval time.Duration) (Status, error) {
	ticker := time.NewTicker(interval)
	defer ticker.Stop()

	for {
		st, err := c.GetStatus(ctx, baseURL)
		if err == nil && st.Streaming {
			return st, nil
		}
		select {
		case <-ctx.Done():
			if err != nil {
				return Status{}, fmt.Errorf("сервер телефона не отвечает (%w)", err)
			}
			return Status{}, ctx.Err()
		case <-ticker.C:
		}
	}
}

// StreamURL returns the raw media endpoint for the given codec.
func StreamURL(baseURL, codec string) string {
	base := strings.TrimRight(baseURL, "/")
	canon, _ := NormalizeCodec(codec)
	switch canon {
	case "h265":
		return base + "/stream.h265"
	case "av1":
		return base + "/stream.av1"
	case "mjpeg":
		return base + "/stream.mjpg"
	default:
		return base + "/stream.h264"
	}
}
