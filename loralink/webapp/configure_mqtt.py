import requests
import sys

# Default settings
BASE_URL = "http://localhost:8000"
MQTT_SRV = "172.16.0.25"  # Your machine's IP on the Wi-Fi network
MQTT_PRT = "1883"

def send_cmd(cmd):
    url = f"{BASE_URL}/api/cmd"
    try:
        r = requests.post(url, json={"cmd": cmd}, timeout=5)
        if r.status_code == 200:
            print(f"  [OK]  {cmd}")
            return True
        else:
            print(f"  [ERR] {cmd} (Status: {r.status_code}, {r.text})")
            return False
    except Exception as e:
        print(f"  [EXC] {cmd} ({e})")
        return False

def main():
    print(f"Connecting to LoRaLink Webapp at {BASE_URL}...")

    # 1. Check if server is up
    try:
        requests.get(BASE_URL, timeout=2)
    except:
        print(f"ERROR: Webapp server not found at {BASE_URL}.")
        print("Please start the server first: 'python server.py --ip <YOUR_DEVICE_IP>'")
        sys.exit(1)

    print(f"Configuring local MQTT broker ({MQTT_SRV}:{MQTT_PRT})...")

    commands = [
        f"CONFIG SET MQTT_SRV {MQTT_SRV}",
        f"CONFIG SET MQTT_PRT {MQTT_PRT}",
        "CONFIG SET MQTT_EN true"
    ]

    success = True
    for cmd in commands:
        if not send_cmd(cmd):
            success = False
            break

    if success:
        print("\nSuccess! Your device is now configured to use the local EMQX broker.")
        print("Note: The device might reboot to apply settings if WiFi/IP was changed.")
        print("Check the MQTT Swarm Dashboard to verify message flow.")
    else:
        print("\nSome commands failed. Ensure your device is connected to the webapp.")

if __name__ == "__main__":
    main()
