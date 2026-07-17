import os
import requests
import time
import urllib3

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

TOKEN = os.environ.get("HOME_ASSISTANT_TOKEN")
HOST = os.environ.get("HOME_ASSISTANT_HOST")
HEADERS = {
    "Authorization": f"Bearer {TOKEN}",
    "Content-Type": "application/json"
}

def set_mode(mode):
    print(f"Setting mode to {mode}...")
    url = f"{HOST}/api/services/select/select_option"
    res = requests.post(url, headers=HEADERS, json={"entity_id": "select.alpha_hwr_pump_control_mode", "option": mode}, verify=False)
    res.raise_for_status()

def set_flow(value):
    print(f"Setting flow setpoint to {value}...")
    url = f"{HOST}/api/services/number/set_value"
    res = requests.post(url, headers=HEADERS, json={"entity_id": "number.alpha_hwr_constant_flow_setpoint", "value": value}, verify=False)
    res.raise_for_status()

def set_switch(entity_id, enabled):
    print(f"Setting {entity_id} to {enabled}...")
    url = f"{HOST}/api/services/switch/turn_{'on' if enabled else 'off'}"
    res = requests.post(url, headers=HEADERS, json={"entity_id": entity_id}, verify=False)
    res.raise_for_status()

def get_state(entity_id):
    url = f"{HOST}/api/states/{entity_id}"
    res = requests.get(url, headers=HEADERS, verify=False)
    res.raise_for_status()
    return res.json().get("state")

print("Waiting for stable connection...")
time.sleep(2)

set_switch("switch.alpha_hwr_schedule_enabled", False)
set_switch("switch.alpha_hwr_temperature_autoadapt", False)
set_switch("switch.alpha_hwr_remote_mode", True)
print("Waiting for remote mode to engage...")
time.sleep(3)

print(f"Current mode: {get_state('select.alpha_hwr_pump_control_mode')}")
set_mode("Constant Flow")
print("Waiting 6 seconds for UI update...")
time.sleep(6)

current_mode = get_state('select.alpha_hwr_pump_control_mode')
print(f"Current mode after set: {current_mode}")

if current_mode == "Constant Flow":
    print("SUCCESS: Mode stuck successfully!")
else:
    print("FAILED: Mode did not stick.")

flow_state = get_state("number.alpha_hwr_constant_flow_setpoint")
print(f"Flow Setpoint is now: {flow_state}")

if flow_state != "unknown":
    print("SUCCESS: UI is not disabled! Attempting to set new value...")
    set_flow(2.5)
    time.sleep(6)
    new_state = get_state("number.alpha_hwr_constant_flow_setpoint")
    print(f"Flow Setpoint after write is: {new_state}")
    if new_state == "2.5":
        print("SUCCESS: Write stuck!")
    else:
        print("FAILED: Write did not stick.")
else:
    print("FAILED: Flow setpoint is still unknown.")

# Clean up
set_mode("Temperature Control")
set_switch("switch.alpha_hwr_remote_mode", False)
set_switch("switch.alpha_hwr_schedule_enabled", True)
set_switch("switch.alpha_hwr_temperature_autoadapt", True)
