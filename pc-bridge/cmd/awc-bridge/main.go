package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"os/signal"
	"runtime"
	"strings"
	"syscall"
	"time"

	"androidwebcamera/pc-bridge/internal/adb"
	"androidwebcamera/pc-bridge/internal/client"
	"androidwebcamera/pc-bridge/internal/v4l2"
)

const version = "1.1.0"

func main() {
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintf(os.Stderr, "Ошибка: %v\n", err)
		os.Exit(1)
	}
}

func run(args []string) error {
	fs := flag.NewFlagSet("awc-bridge", flag.ContinueOnError)
	mode := fs.String("mode", "usb", "Режим подключения: usb (adb forward) или wifi")
	phoneURL := fs.String("url", "", "Базовый URL телефона в режиме wifi, например http://192.168.1.42:8080")
	port := fs.Int("port", 8080, "Порт сервера на телефоне и локального проброса (сокращение для --local-port и --remote-port)")
	localPort := fs.Int("local-port", 0, "Локальный TCP-порт на ПК (по умолчанию равен --port)")
	remotePort := fs.Int("remote-port", 0, "Удалённый TCP-порт на телефоне (по умолчанию равен --port)")
	serial := fs.String("serial", "", "Serial Android-устройства из 'adb devices' (если подключено несколько)")
	v4l2Spec := fs.String("v4l2", "", "Путь к виртуальной камере Linux (/dev/videoX или auto для автопоиска v4l2loopback)")
	rotateFlag := fs.String("rotate", "auto", "Поворот для v4l2: auto (как на телефоне), 0, 90, 180, 270")
	mirrorFlag := fs.String("mirror", "auto", "Отражение по горизонтали для v4l2: auto, true, false")
	codecFlag := fs.String("codec", "auto", "Предпочтительный кодек потока: auto, h264, h265, av1, mjpeg")
	adbPath := fs.String("adb", "", "Явный путь к бинарнику adb")
	ffmpegPath := fs.String("ffmpeg", "", "Явный путь к бинарнику ffmpeg")
	openBrowser := fs.Bool("open", false, "Автоматически открыть веб-интерфейс в браузере")
	showVersion := fs.Bool("version", false, "Показать версию")

	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return nil
		}
		return err
	}

	if *showVersion {
		fmt.Printf("awc-bridge %s (%s/%s)\n", version, runtime.GOOS, runtime.GOARCH)
		return nil
	}

	var requestedCodec string
	if !strings.EqualFold(strings.TrimSpace(*codecFlag), "auto") && strings.TrimSpace(*codecFlag) != "" {
		canon, ok := client.NormalizeCodec(*codecFlag)
		if !ok {
			return fmt.Errorf("неизвестный --codec %q (допустимо: auto, h264, h265, av1, mjpeg)", *codecFlag)
		}
		requestedCodec = canon
	}

	lPort := *localPort
	if lPort == 0 {
		lPort = *port
	}
	rPort := *remotePort
	if rPort == 0 {
		rPort = *port
	}

	ctx, stopSignals := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stopSignals()

	var baseURL string
	var adbMgr *adb.Manager
	var activeSerial string

	switch strings.ToLower(strings.TrimSpace(*mode)) {
	case "usb":
		adbBin, err := adb.FindAdb(*adbPath)
		if err != nil {
			return err
		}
		adbMgr = adb.NewManager(adbBin)
		dev, err := adbMgr.PickOnlineDevice(ctx, *serial)
		if err != nil {
			return err
		}
		activeSerial = dev.Serial
		if err := adbMgr.Forward(ctx, activeSerial, lPort, rPort); err != nil {
			return fmt.Errorf("не удалось настроить adb forward: %w", err)
		}
		defer func() {
			cleanCtx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
			defer cancel()
			_ = adbMgr.RemoveForward(cleanCtx, activeSerial, lPort)
		}()

		baseURL = fmt.Sprintf("http://127.0.0.1:%d", lPort)
		modelLabel := dev.Model
		if modelLabel == "" {
			modelLabel = activeSerial
		}
		fmt.Printf("[USB] Подключено устройство %s (%s) -> %s\n", modelLabel, activeSerial, baseURL)

	case "wifi":
		if strings.TrimSpace(*phoneURL) == "" {
			return errors.New("для --mode wifi укажите адрес телефона через --url, например --url http://192.168.1.42:8080")
		}
		baseURL = strings.TrimRight(strings.TrimSpace(*phoneURL), "/")
		if !strings.HasPrefix(baseURL, "http://") && !strings.HasPrefix(baseURL, "https://") {
			baseURL = "http://" + baseURL
		}
		fmt.Printf("[Wi-Fi] Подключение к %s\n", baseURL)

	default:
		return fmt.Errorf("неизвестный --mode %q (используйте usb или wifi)", *mode)
	}

	api := client.New()

	fmt.Println("Ожидание активной трансляции на телефоне (нажмите «Начать трансляцию» в AndroidWebCamera)...")
	status, err := api.WaitForReady(ctx, baseURL, 750*time.Millisecond)
	if err != nil {
		if ctx.Err() != nil {
			return nil
		}
		return err
	}

	if requestedCodec != "" && !strings.EqualFold(status.Codec, requestedCodec) {
		updated, err := api.UpdateConfig(ctx, baseURL, map[string]any{"codec": requestedCodec})
		if err != nil {
			return fmt.Errorf("не удалось переключить кодек на %s: %w", requestedCodec, err)
		}
		status = updated
	}

	printConnectionSummary(baseURL, status)

	if *openBrowser {
		openURL(baseURL + "/")
	}

	if strings.TrimSpace(*v4l2Spec) != "" {
		dev, err := v4l2.ResolveDevice(*v4l2Spec)
		if err != nil {
			return err
		}
		ffmpegBin, err := v4l2.FindFFmpeg(*ffmpegPath)
		if err != nil {
			return err
		}
		if dev.Label != "" {
			fmt.Printf("[v4l2] Выбрана виртуальная камера: %s (%s)\n", dev.Path, dev.Label)
		} else {
			fmt.Printf("[v4l2] Выбрана виртуальная камера: %s\n", dev.Path)
		}

		return v4l2.RunPipeline(ctx, ffmpegBin, func(reqCtx context.Context) (v4l2.Options, error) {
			st, err := api.WaitForReady(reqCtx, baseURL, 500*time.Millisecond)
			if err != nil {
				return v4l2.Options{}, err
			}
			if requestedCodec != "" && !strings.EqualFold(st.Codec, requestedCodec) {
				updated, err := api.UpdateConfig(reqCtx, baseURL, map[string]any{"codec": requestedCodec})
				if err != nil {
					return v4l2.Options{}, fmt.Errorf("не удалось переключить кодек на %s: %w", requestedCodec, err)
				}
				st = updated
			}
			rot, err := v4l2.ResolveRotation(*rotateFlag, st.Rotation)
			if err != nil {
				return v4l2.Options{}, err
			}
			mirror, err := resolveMirror(*mirrorFlag, st.Mirrored)
			if err != nil {
				return v4l2.Options{}, err
			}
			return v4l2.Options{
				StreamURL:  client.StreamURL(baseURL, st.Codec),
				Codec:      st.Codec,
				Device:     dev.Path,
				Rotation:   rot,
				Mirror:     mirror,
				FFmpegPath: ffmpegBin,
			}, nil
		}, os.Stderr)
	}

	fmt.Println("Мост активен. Нажмите Ctrl+C для завершения.")
	<-ctx.Done()
	fmt.Println("\nОстановка awc-bridge...")
	return nil
}

func resolveMirror(flagVal string, statusMirror bool) (bool, error) {
	switch strings.ToLower(strings.TrimSpace(flagVal)) {
	case "", "auto":
		return statusMirror, nil
	case "true", "1", "yes":
		return true, nil
	case "false", "0", "no":
		return false, nil
	default:
		return false, fmt.Errorf("некорректное значение --mirror %q (допустимо: auto, true, false)", flagVal)
	}
}

func printConnectionSummary(baseURL string, st client.Status) {
	fmt.Println("------------------------------------------------------------")
	fmt.Printf("Камера:       %s (%dx%d @ %d fps, %s)\n", st.CameraName, st.Width, st.Height, st.FPS, strings.ToUpper(st.Codec))
	fmt.Printf("Веб-плеер:    %s/\n", baseURL)
	fmt.Printf("OBS / VLC:    %s\n", client.StreamURL(baseURL, st.Codec))
	fmt.Printf("MJPEG URL:    %s/stream.mjpg\n", baseURL)
	fmt.Println("------------------------------------------------------------")
}

func openURL(url string) {
	var cmd *exec.Cmd
	switch runtime.GOOS {
	case "windows":
		cmd = exec.Command("cmd", "/c", "start", "", url)
	case "darwin":
		cmd = exec.Command("open", url)
	default:
		cmd = exec.Command("xdg-open", url)
	}
	_ = cmd.Start()
}
