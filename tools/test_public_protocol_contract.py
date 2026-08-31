from __future__ import annotations

import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
WORKSPACE = ROOT.parent


def require(text: str, value: str, source: str) -> None:
    if value not in text:
        raise AssertionError(f"{value!r} missing from {source}")


def main() -> None:
    command_source = (ROOT / "app/Core/User/Src/mqtt_app_cmd.c").read_text(encoding="utf-8")
    heartbeat_source = (ROOT / "app/Core/User/Src/mqtt_app_hb.c").read_text(encoding="utf-8")
    mqtt_header = (ROOT / "app/Core/User/Inc/mqtt_app.h").read_text(encoding="utf-8")
    version_header = (ROOT / "app/Core/User/Inc/app_version.h").read_text(encoding="utf-8")

    require(mqtt_header, '#define MQTT_APP_PUBLIC_VERSION_STR "3.0"', "mqtt_app.h")
    require(mqtt_header, '#define MQTT_APP_INTERNAL_VERSION_STR "2.0"', "mqtt_app.h")
    require(command_source, '#define PUBLIC_DEVICE_TYPE "wing_actuator"', "mqtt_app_cmd.c")
    require(command_source, 'build_response(RESP_CTRL, request_text, 4004U, WING_ACTION_NONE, 1U, 0)', "mqtt_app_cmd.c")
    require(command_source, 'strcmp(device_type->valuestring, PUBLIC_DEVICE_TYPE)', "mqtt_app_cmd.c")
    require(command_source, 'if (result) { cJSON_Delete(result); result = 0; }', "mqtt_app_cmd.c")
    require(heartbeat_source, 'cJSON_AddStringToObject(data, "deviceType", "wing_actuator")', "mqtt_app_hb.c")
    require(heartbeat_source, 'cJSON_AddStringToObject(data, "reportReason", report_reason)', "mqtt_app_hb.c")
    if 'cJSON_AddStringToObject(data, "reason"' in heartbeat_source:
        raise AssertionError("public status still sends legacy reason alias")
    require(version_header, '"top-flying-wing-1.0.14"', "app_version.h")
    require(heartbeat_source, "static char status_json_buf[MQTT_APP_JSON_BUF_SZ]", "mqtt_app_hb.c")
    require(heartbeat_source, "MQTT_Core_PublishEx(g_mqtt_app_status_report_topic, status_json_buf", "mqtt_app_hb.c")

    protocol_paths = [
        WORKSPACE / "ABox_Platform/docs/protocols/A-BOX设备公共MQTT协议-V3.0.md",
        ROOT / "docs/protocols/A-BOX飞翼设备业务协议-V3.0.md",
    ]
    protocol = "\n".join(path.read_text(encoding="utf-8") for path in protocol_paths)
    require(protocol, "wing_actuator", str(protocol_paths[1]))
    require(protocol, "200 ms", str(protocol_paths[1]))
    blocks = re.findall(r"```json\s*(.*?)\s*```", protocol, flags=re.S)
    if not blocks:
        raise AssertionError("public protocol has no JSON examples")
    for index, block in enumerate(blocks, 1):
        root = json.loads(block)
        data = root.get("data")
        if root.get("version") != "3.0":
            raise AssertionError(f"JSON block {index} is not public V3")
        if not isinstance(data, dict) or not str(data.get("deviceType", "")).strip():
            raise AssertionError(f"JSON block {index} has no deviceType")
        if "product" in data:
            raise AssertionError(f"JSON block {index} still contains legacy product")
        if "result" in data:
            raise AssertionError(f"JSON block {index} still contains result wrapper")
    print(f"V3 public protocol contract ok: {len(blocks)} JSON blocks")


if __name__ == "__main__":
    main()
