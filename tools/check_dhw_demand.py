#!/usr/bin/env python3
"""
Diagnostic tool to fetch current sensor values from Home Assistant and check
against dhw_demand detection thresholds.
"""

import os
import sys
import json
import math
from pathlib import Path
from dotenv import load_dotenv
import requests
from datetime import datetime

# Load .env
env_path = Path(__file__).parent.parent / ".env"
load_dotenv(env_path)

HA_HOST = os.getenv("HOME_ASSISTANT_HOST")
HA_TOKEN = os.getenv("HOME_ASSISTANT_TOKEN")

if not HA_HOST or not HA_TOKEN:
    print("ERROR: HOME_ASSISTANT_HOST and HOME_ASSISTANT_TOKEN not set in .env")
    sys.exit(1)

# Remove trailing slash if present
HA_HOST = HA_HOST.rstrip("/")

# Thresholds from dhw_demand.h (defaults)
THRESHOLDS = {
    "pump_off_current_threshold": 0.03,  # A
    "flow_threshold": 0.3,  # GPM
    "thermal_collapse_rate": 0.05,  # °F/s
    "dhw_charge_drop_rate": 0.005,  # %/s
    "inlet_pressure_transient_threshold": 0.07,  # PSI/s
    "inlet_pressure_demand_floor": 5.0,  # PSI
    "pump_flow_collapse_threshold": 0.2,  # GPM
    "motor_current_spike_threshold": 0.001,  # A/s
    "pump_power_spike_threshold": 5.0,  # W/s
    "pump_head_rate_threshold": 0.31,  # m/s
    "flow_latch_seconds": 30,  # s
    "session_gap_tolerance_seconds": 60,  # s
}

# Sensor entity IDs (from dhw-demand-example.yaml)
ENTITY_IDS = {
    "motor_speed": "sensor.alpha_hwr_motor_speed",  # RPM
    "motor_current": "sensor.alpha_hwr_motor_current",  # A
    "inlet_pressure_psi": "sensor.alpha_hwr_inlet_pressure",  # PSI (converted)
    "pump_flow_gpm": "sensor.alpha_hwr_flow_rate",  # GPM (converted)
    "pump_power": "sensor.alpha_hwr_power",  # W
    "pump_head_rate": "sensor.alpha_hwr_head_rate",  # m/s
    "flow": "sensor.droplet_f5bc_flow_rate",  # GPM (Droplet)
    "tank_lower_temp": "sensor.nwp500_tank_lower_temperature",  # °F
    "dhw_charge": "sensor.nwp500_dhw_charge",  # %
    "dhw_in_use": "sensor.nwp500_dhw_in_use",  # bool
    "dhw_demand": "binary_sensor.dhw_detector_dhw_demand",  # Current demand state
    "demand_confidence": "sensor.dhw_detector_dhw_detection_confidence",  # Confidence
    "demand_method": "text_sensor.dhw_detector_dhw_detection_method",  # Method
    "session_duration": "sensor.dhw_detector_dhw_session_duration",  # Session seconds
}


def get_state(entity_id):
    """Fetch the current state of an entity from HA."""
    url = f"{HA_HOST}/api/states/{entity_id}"
    headers = {"Authorization": f"Bearer {HA_TOKEN}", "Content-Type": "application/json"}
    
    try:
        resp = requests.get(url, headers=headers, timeout=5)
        if resp.status_code == 200:
            data = resp.json()
            return data.get("state"), data.get("attributes", {})
        elif resp.status_code == 404:
            return None, {}
        else:
            print(f"ERROR fetching {entity_id}: {resp.status_code} {resp.text}")
            return None, {}
    except Exception as e:
        print(f"ERROR fetching {entity_id}: {e}")
        return None, {}


def main():
    print("=" * 80)
    print(f"DHW Demand Detector — Sensor Diagnostic")
    print(f"Time: {datetime.now().isoformat()}")
    print(f"Home Assistant: {HA_HOST}")
    print("=" * 80)
    print()

    # Fetch all sensor values
    values = {}
    for name, entity_id in ENTITY_IDS.items():
        state, attrs = get_state(entity_id)
        values[name] = state
        
        # Print with unit if available
        unit = attrs.get("unit_of_measurement", "")
        print(f"{name:30} {entity_id:50}")
        if state is None:
            print(f"  → UNAVAILABLE")
        else:
            try:
                val = float(state) if state != "off" and state != "on" else state
                if unit:
                    print(f"  → {val} {unit}")
                else:
                    print(f"  → {val}")
            except:
                print(f"  → {state}")
        print()

    print("=" * 80)
    print("DETECTION STATE")
    print("=" * 80)
    print(f"Demand:         {values.get('dhw_demand', 'unknown')}")
    print(f"Confidence:     {values.get('demand_confidence', 'unknown')}")
    print(f"Method:         {values.get('demand_method', 'unknown')}")
    print(f"Session (s):    {values.get('session_duration', 'unknown')}")
    print()

    print("=" * 80)
    print("DIAGNOSTIC CHECK")
    print("=" * 80)

    # Pump state
    try:
        motor_speed = float(values.get("motor_speed")) if values.get("motor_speed") else None
        motor_current = float(values.get("motor_current")) if values.get("motor_current") else None
    except:
        motor_speed = None
        motor_current = None

    pump_on = False
    if motor_speed is not None:
        pump_on = motor_speed >= 10.0
        print(f"Motor Speed: {motor_speed} RPM → pump_on={pump_on}")
    elif motor_current is not None:
        pump_on = motor_current >= THRESHOLDS["pump_off_current_threshold"]
        print(f"Motor Current: {motor_current} A (threshold {THRESHOLDS['pump_off_current_threshold']}) → pump_on={pump_on}")
    else:
        print("Motor Speed and Current: BOTH UNAVAILABLE")

    print()
    print(f"PUMP STATE: {'ON' if pump_on else 'OFF'}")
    print()

    if pump_on:
        print("PUMP-ON DETECTION PATH")
        print("-" * 80)
        
        # Check continuation path
        try:
            flow = float(values.get("flow")) if values.get("flow") else None
        except:
            flow = None
        
        if flow is not None:
            print(f"Droplet flow: {flow} GPM (threshold {THRESHOLDS['flow_threshold']})")
            if flow > THRESHOLDS["flow_threshold"]:
                print("  → Continuation path eligible (if demand was active before pump started)")
            else:
                print("  → Below threshold for continuation detection")
        else:
            print("Droplet flow: UNAVAILABLE")
        
        print()
        print("Pump-on deterministic voting:")
        
        votes = []
        
        # Check signals
        try:
            inlet_psi = float(values.get("inlet_pressure_psi")) if values.get("inlet_pressure_psi") else None
        except:
            inlet_psi = None
        
        if inlet_psi is not None:
            print(f"  Inlet pressure: {inlet_psi} PSI (floor threshold {THRESHOLDS['inlet_pressure_demand_floor']})")
            if inlet_psi < THRESHOLDS["inlet_pressure_demand_floor"]:
                votes.append("pressure_floor")
                print(f"    ✓ VOTE: pressure below floor")
            else:
                print(f"    ✗ above floor")
        else:
            print(f"  Inlet pressure: UNAVAILABLE")
        
        try:
            pump_flow_gpm = float(values.get("pump_flow_gpm")) if values.get("pump_flow_gpm") else None
        except:
            pump_flow_gpm = None
        
        if pump_flow_gpm is not None:
            print(f"  Pump flow: {pump_flow_gpm} GPM (collapse threshold {THRESHOLDS['pump_flow_collapse_threshold']})")
            if pump_flow_gpm < THRESHOLDS["pump_flow_collapse_threshold"]:
                votes.append("flow_collapse")
                print(f"    ✓ VOTE: flow collapsed")
            else:
                print(f"    ✗ above threshold")
        else:
            print(f"  Pump flow: UNAVAILABLE")
        
        try:
            pump_power = float(values.get("pump_power")) if values.get("pump_power") else None
        except:
            pump_power = None
        
        if pump_power is not None:
            print(f"  Pump power: {pump_power} W")
        else:
            print(f"  Pump power: UNAVAILABLE")
        
        print(f"\nVotes: {len(votes)} ({', '.join(votes) if votes else 'NONE'})")
        if votes:
            print(f"  → Demand would be detected (confidence based on vote count)")
        else:
            print(f"  → No demand detected (insufficient votes)")
    
    else:
        print("PUMP-OFF DETECTION PATH")
        print("-" * 80)
        
        # Check flow
        try:
            flow = float(values.get("flow")) if values.get("flow") else None
        except:
            flow = None
        
        signals = []
        
        if flow is not None:
            print(f"Droplet flow: {flow} GPM (threshold {THRESHOLDS['flow_threshold']})")
            if flow > THRESHOLDS["flow_threshold"]:
                signals.append(("flow", 1.0))
                print(f"  ✓ SIGNAL: deterministic_flow (weight 1.0)")
            else:
                print(f"  ✗ below threshold")
        else:
            print(f"Droplet flow: UNAVAILABLE")
        
        # Check thermal
        try:
            tank_temp = float(values.get("tank_lower_temp")) if values.get("tank_lower_temp") else None
        except:
            tank_temp = None
        
        print(f"Tank lower temp: {tank_temp} °F" if tank_temp is not None else "Tank lower temp: UNAVAILABLE")
        
        # Check DHW charge
        try:
            dhw_charge = float(values.get("dhw_charge")) if values.get("dhw_charge") else None
        except:
            dhw_charge = None
        
        print(f"DHW charge: {dhw_charge} %" if dhw_charge is not None else "DHW charge: UNAVAILABLE")
        
        print(f"\nSignals: {len(signals)}")
        for sig, weight in signals:
            print(f"  • {sig} (weight {weight})")
        
        if signals:
            print(f"\n  → Demand would be detected")
        else:
            print(f"\n  → No demand detected (no signals or no-flow guard active)")


if __name__ == "__main__":
    main()
