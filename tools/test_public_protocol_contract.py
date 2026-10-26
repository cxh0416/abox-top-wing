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
            assert data["command"] in ("wing_control", "sync_state"), index
            if data["command"] == "wing_control":
                action = data["params"]["action"]
                assert action in ("ascend", "descend", "stop"), index
                if action != "stop":
                    assert 0 < data["expiresAt"] - obj["timestamp"] <= 5000, index
                else:
                    assert "expiresAt" not in data, index
            else:
                assert data["params"] is None and "expiresAt" not in data, index
        if "reportName" in data:
            assert data["reportName"] in ("wing_status", "wing_position"), index
            if data["reportName"] == "wing_status":
                assert data["reportType"] == "state" and set(data["payload"]) == fields, index
            else:
                assert data["reportType"] == "telemetry" and set(data["payload"]) == {"positionMm", "statusFresh"}, index
        if "profiles" in data:
            assert data["profiles"] == [{"name": "wing", "version": "1.0"}], index
    print(f"V4 public protocol examples checked: {len(blocks)}")


if __name__ == "__main__":
    main()
