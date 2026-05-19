import argparse
import asyncio
import socket
import subprocess
import sys


DEVICE_NAME = "ESP32-Pomodoro-Setup"
SERVICE_UUID = "8cb3b7a0-6f8b-4e6e-8a19-8f823c76f101"
RX_UUID = "8cb3b7a1-6f8b-4e6e-8a19-8f823c76f101"
TX_UUID = "8cb3b7a2-6f8b-4e6e-8a19-8f823c76f101"
BleakClient = None
BleakScanner = None


def load_bleak():
    global BleakClient, BleakScanner

    if BleakClient is not None and BleakScanner is not None:
        return

    try:
        from bleak import BleakClient as ImportedBleakClient
        from bleak import BleakScanner as ImportedBleakScanner
    except ImportError:
        print("Missing dependency: bleak. Install it with: pip install bleak", file=sys.stderr)
        raise SystemExit(1)

    BleakClient = ImportedBleakClient
    BleakScanner = ImportedBleakScanner


def build_payload(args):
    lines = [
        f"ssid={args.ssid}",
        f"wifi_password={args.wifi_password}",
        f"mqtt_broker={args.mqtt_broker}",
        f"mqtt_port={args.mqtt_port}",
        f"mqtt_username={args.mqtt_username or ''}",
        f"mqtt_password={args.mqtt_password or ''}",
        f"mqtt_topic={args.mqtt_topic}",
        f"mqtt_client_id={args.mqtt_client_id}",
    ]

    if args.device_ip:
        lines.extend([
            f"device_ip={args.device_ip}",
            f"gateway={args.gateway}",
            f"subnet={args.subnet}",
            f"dns={args.dns or ''}",
        ])

    if args.clear_static_ip:
        lines.append("clear_static_ip=1")

    return "\n".join(lines) + "\n"


def detect_windows_wifi_ssid():
    result = subprocess.run(
        ["netsh", "wlan", "show", "interfaces"],
        capture_output=True,
        text=True,
        errors="ignore",
        check=False,
    )

    if result.returncode != 0:
        raise RuntimeError("Could not read Wi-Fi interface info with netsh")

    for line in result.stdout.splitlines():
        key, separator, value = line.partition(":")
        if separator and key.strip() == "SSID":
            ssid = value.strip()
            if ssid:
                return ssid

    raise RuntimeError("Could not detect current Wi-Fi SSID")


def detect_default_ipv4():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect(("8.8.8.8", 80))
        return sock.getsockname()[0]
    finally:
        sock.close()


def apply_auto_windows_config(args):
    if args.reset or not args.auto_windows:
        return

    if not args.ssid:
        args.ssid = detect_windows_wifi_ssid()
        print(f"Detected Wi-Fi SSID: {args.ssid}")

    if not args.mqtt_broker:
        args.mqtt_broker = detect_default_ipv4()
        print(f"Detected MQTT broker IP: {args.mqtt_broker}")


async def find_device(address):
    if address:
        return address

    print(f"Scanning for {DEVICE_NAME}...")
    devices = await BleakScanner.discover(timeout=10.0, service_uuids=[SERVICE_UUID])

    for device in devices:
        if device.name == DEVICE_NAME:
            return device.address

    devices = await BleakScanner.discover(timeout=10.0)
    for device in devices:
        if device.name == DEVICE_NAME:
            return device.address

    raise RuntimeError(f"Could not find BLE device named {DEVICE_NAME}")


async def provision(args):
    load_bleak()

    address = await find_device(args.address)
    payload = "reset\n" if args.reset else build_payload(args)

    print(f"Connecting to {address}...")

    def on_notify(_, data):
        print("ESP32:", data.decode(errors="replace").strip())

    async with BleakClient(address) as client:
        await client.start_notify(TX_UUID, on_notify)
        await asyncio.sleep(0.5)
        await client.write_gatt_char(RX_UUID, payload.encode("utf-8"), response=True)
        await asyncio.sleep(3.0)
        try:
            if client.is_connected:
                await client.stop_notify(TX_UUID)
        except Exception:
            pass


def parse_args():
    parser = argparse.ArgumentParser(description="Provision ESP32 Pomodoro Cube over BLE.")
    parser.add_argument("--address", help="BLE address. If omitted, the script scans by device name.")
    parser.add_argument("--ssid", help="Wi-Fi SSID.")
    parser.add_argument("--wifi-password", default="", help="Wi-Fi password. Leave empty for open Wi-Fi.")
    parser.add_argument("--mqtt-broker", help="MQTT broker hostname or IP.")
    parser.add_argument("--mqtt-port", type=int, default=1883, help="MQTT broker port.")
    parser.add_argument("--mqtt-username", default="", help="MQTT username.")
    parser.add_argument("--mqtt-password", default="", help="MQTT password.")
    parser.add_argument("--mqtt-topic", default="pomodoro/cube/state/321", help="MQTT topic.")
    parser.add_argument("--mqtt-client-id", default="esp32_pomodoro_cube", help="MQTT client id.")
    parser.add_argument("--device-ip", help="Optional static IP for ESP32.")
    parser.add_argument("--gateway", default="", help="Gateway for static IP.")
    parser.add_argument("--subnet", default="", help="Subnet mask for static IP.")
    parser.add_argument("--dns", default="", help="Optional DNS for static IP.")
    parser.add_argument("--clear-static-ip", action="store_true", help="Switch ESP32 back to DHCP.")
    parser.add_argument(
        "--auto-windows",
        action="store_true",
        help="Detect the current Windows Wi-Fi SSID and PC IPv4 address automatically.",
    )
    parser.add_argument("--reset", action="store_true", help="Clear saved ESP32 config.")

    args = parser.parse_args()
    apply_auto_windows_config(args)

    if not args.reset:
        missing = []
        if not args.ssid:
            missing.append("--ssid")
        if not args.mqtt_broker:
            missing.append("--mqtt-broker")
        if args.device_ip and (not args.gateway or not args.subnet):
            missing.extend(["--gateway", "--subnet"])
        if missing:
            parser.error("missing required arguments: " + ", ".join(missing))

    return args


def main():
    args = parse_args()
    asyncio.run(provision(args))


if __name__ == "__main__":
    main()
