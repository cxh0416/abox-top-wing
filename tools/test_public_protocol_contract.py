"""Check product V4 examples, envelope and Profile fields."""
from __future__ import annotations
import json
import re
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    path = ROOT / "docs/protocols/A-BOX飞翼设备业务协议-V4.0.md"
    text = path.read_text(encoding="utf-8")
    blocks = re.findall(r"```json\s*(.*?)\s*```", text, re.S)
    assert blocks, "no public V4 examples"
    fields = {"motionState", "activeAction", "positionMm", "topLimitMm", "bottomLimitMm",
              "atEndpoint", "faultCode", "controllerOnline", "statusFresh", "safetyStopReason"}
    for index, block in enumerate(blocks, 1):
        obj = json.loads(block)
        assert obj["version"] == "4.0", index
        assert type(obj["timestamp"]) is int and 0 <= obj["timestamp"] <= 9007199254740991, index
        data = obj["data"]
        assert isinstance(data, dict) and "deviceType" not in data and "product" not in data, index
        assert len(block.encode("utf-8")) <= 1024, index
        for field in ("params", "result", "payload"):
            if field in data:
                assert len(json.dumps(data[field], ensure_ascii=False, separators=(",", ":")).encode()) <= 768, index
        if "command" in data:
            assert data["command"] in ("wing_control", "sync_state", "power_on", "power_off"), index
            if data["command"] == "wing_control":
                action = data["params"]["action"]
                assert action in ("ascend", "descend", "stop"), index
                if action != "stop":
                    assert 0 < data["expiresAt"] - obj["timestamp"] <= 5000, index
                else:
                    assert "expiresAt" not in data, index
            else:
                assert data["params"] is None, index
                if data["command"] == "power_on":
                    assert 0 < data["expiresAt"] - obj["timestamp"] <= 5000, index
                elif data["command"] == "sync_state":
                    assert "expiresAt" not in data, index
                elif "expiresAt" in data:
                    assert data["expiresAt"] > obj["timestamp"], index
        if "reportName" in data:
            assert data["reportName"] in ("wing_status", "wing_position", "power_status"), index
            if data["reportName"] == "wing_status":
                assert data["reportType"] == "state" and set(data["payload"]) == fields, index
            elif data["reportName"] == "wing_position":
                assert data["reportType"] == "telemetry" and set(data["payload"]) == {"positionMm", "statusFresh"}, index
            else:
                assert data["reportType"] == "state" and set(data["payload"]) == {"powerOutputOn"}, index
                assert type(data["payload"]["powerOutputOn"]) is bool, index
        if isinstance(data.get("result"), dict) and "powerOutputOn" in data["result"]:
            assert data["code"] == 200 and type(data["result"]["powerOutputOn"]) is bool, index
        if "profiles" in data:
            profiles = data["profiles"]
            assert all(p in ({"name": "wing", "version": "1.0"},
                             {"name": "device_power", "version": "1.0"}) for p in profiles), index
            assert sum(p["name"] == "wing" for p in profiles) == 1, index
            assert sum(p["name"] == "device_power" for p in profiles) <= 1, index
    print(f"V4 public protocol examples checked: {len(blocks)}")


if __name__ == "__main__":
    main()
