from pathlib import Path


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    main_source = (root / "app/Core/Src/main.c").read_text(encoding="utf-8")
    uart_source = (root / "app/Core/Src/usart.c").read_text(encoding="utf-8")
    ioc = (root / "app/TopFlyingWing_App.ioc").read_text(encoding="utf-8")
    cmake = (root / "app/CMakeLists.txt").read_text(encoding="utf-8")
    rx_header = (root / "platform/components/ec800/include/abox_ec800_rx.h").read_text(encoding="utf-8")
    at_header = (root / "platform/components/ec800/include/abox_ec800_at.h").read_text(encoding="utf-8")

    assert "Dma.USART1_RX.0.Mode=DMA_CIRCULAR" in ioc
    assert "hdma_usart1_rx.Init.Mode = DMA_CIRCULAR" in uart_source
    assert "#define UART1_RX_BUF_SZ 1088U" in main_source
    assert "#define UART1_RX_RING_SZ 1152U" in main_source
    assert "ABoxEc800Rx_OnDmaEvent(&g_ec800_rx, Size)" in main_source
    assert "ABoxEc800Rx_Drain(&g_ec800_rx" in main_source
    assert "ABoxEc800At_AbortAll(&g_ec800_at, ABOX_EC800_RESULT_ERROR)" in main_source
    assert "EC800_AT_SetRxOverflowCount" in main_source
    assert "ABoxEc800Rx_AfterRestart(&g_ec800_rx)" in main_source
    assert "ABOX_EC800_RX_DMA_MIN 1088U" in rx_header
    assert "ABOX_EC800_RX_RING_MIN 1152U" in rx_header
    assert "ABOX_EC800_AT_RAW_MAX 1024U" in at_header
    assert "Core/User/Src/ec800_at.c" not in cmake
    assert "abox::ec800" in cmake
    assert not (root / "app/Core/User/Src/ec800_at.c").exists()
    print("uart1_circular_dma_contract: PASS")


if __name__ == "__main__":
    main()
