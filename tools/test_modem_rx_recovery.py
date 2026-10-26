"""Compile the actual main.c RX service against HAL failure-injection boundaries."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
source = (root / "app/Core/Src/main.c").read_text(encoding="utf-8")
start = source.index("static void Ec800RxService(void)\n{")
end = source.index("\n}\n", start) + 3
build = root / "build/host"
build.mkdir(parents=True, exist_ok=True)
(build / "modem_rx_service.inc").write_text(source[start:end], encoding="utf-8", newline="\n")
exe = build / "test_modem_rx_recovery.exe"
subprocess.run([sys.argv[1], "-std=c11", "-Wall", "-Wextra", "-Werror", "-UNDEBUG",
                "-I", str(build), str(root / "tests/test_modem_rx_recovery.c"),
                "-o", str(exe)], check=True)
subprocess.run([str(exe)], check=True)
