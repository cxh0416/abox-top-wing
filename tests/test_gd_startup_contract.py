from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
public = (ROOT / "platform/hardware/mcu/gd32f105/startup_gd32f105.c").read_text(encoding="utf-8")
product = (ROOT / "app/Core/User/gd32/startup_gd32_wing.c").read_text(encoding="utf-8")
expected = public.replace("extern int main(void);", "extern int main(void);\nextern void WingGd_SysTick(void);")
expected = expected.replace("    [3] = (uintptr_t)HardFault_Handler,",
                            "    [3] = (uintptr_t)HardFault_Handler,\n    [15] = (uintptr_t)WingGd_SysTick,")
assert product == expected, "GD startup must track the platform vector table plus the product safety tick"
assert "AntiPinchBoard_Tick(HAL_GetTick())" in (ROOT / "app/Core/User/gd32/main_gd32.c").read_text(encoding="utf-8")
print("GD startup vector and 1 ms safety tick contract passed")
