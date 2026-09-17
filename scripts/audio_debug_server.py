import socket
import wave
import argparse
import time
from pathlib import Path


def main(samplerate, duration, output, port, wait_timeout, stall_timeout):
    channels = 1
    bytes_per_frame = channels * 2
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    server_socket.bind(('0.0.0.0', port))
    server_socket.settimeout(1.0)

    output_path = Path(output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    target_frames = round(samplerate * duration)
    frames_written = 0
    wav_file = None
    wait_started_at = time.monotonic()
    last_packet_at = None
    next_progress_second = 1

    print(f"Listening on 0.0.0.0:{port}; waiting for {duration:g}s of wake-inference mono PCM")

    try:
        while frames_written < target_frames:
            try:
                message, address = server_socket.recvfrom(65535)
            except socket.timeout:
                now = time.monotonic()
                if wav_file is None:
                    if now - wait_started_at >= wait_timeout:
                        print(f"No PCM received within {wait_timeout:g} seconds")
                        return 1
                    continue
                if now - last_packet_at >= stall_timeout:
                    print(f"PCM stream stopped for {stall_timeout:g} seconds")
                    return 1
                print("Waiting for AFE output to resume...")
                continue

            if wav_file is None:
                wav_file = wave.open(str(output_path), "wb")
                wav_file.setnchannels(channels)
                wav_file.setsampwidth(2)
                wav_file.setframerate(samplerate)
                print(f"Recording from {address} to {output_path}")

            last_packet_at = time.monotonic()
            complete_bytes = len(message) - (len(message) % bytes_per_frame)
            remaining_bytes = (target_frames - frames_written) * bytes_per_frame
            chunk = message[:min(complete_bytes, remaining_bytes)]
            wav_file.writeframesraw(chunk)
            frames_written += len(chunk) // bytes_per_frame

            recorded_seconds = frames_written / samplerate
            if recorded_seconds >= next_progress_second:
                print(f"Captured {recorded_seconds:.1f}/{duration:g}s")
                next_progress_second += 1

    except KeyboardInterrupt:
        print("\nRecording stopped early")
        return 1
    finally:
        if wav_file is not None:
            wav_file.close()
        server_socket.close()

    print(f"Saved {frames_written / samplerate:.2f}s to {output_path}")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='保存实际送入唤醒模型的单声道 PCM 为 WAV')
    parser.add_argument('--samplerate', '-s', type=int, default=16000, 
                        help='采样率 (默认: 16000)')
    parser.add_argument('--duration', '-d', type=float, default=20.0,
                        help='录音时长，单位秒 (默认: 20)')
    parser.add_argument('--output', '-o', default='tmp/wake_inference_input.wav',
                        help='输出 WAV 路径')
    parser.add_argument('--port', '-p', type=int, default=8000,
                        help='UDP 监听端口 (默认: 8000)')
    parser.add_argument('--wait-timeout', type=float, default=120.0,
                        help='等待首次进入录音状态的最长秒数 (默认: 120)')
    parser.add_argument('--stall-timeout', type=float, default=30.0,
                        help='收到首包后允许 AFE 暂停的最长秒数 (默认: 30)')
    
    args = parser.parse_args()
    raise SystemExit(main(args.samplerate, args.duration, args.output, args.port,
                          args.wait_timeout, args.stall_timeout))
