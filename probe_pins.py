#!/usr/bin/env python3
"""MiliTV TFT SPI bus init probe: read SPI2 registers from live device RAM via gdbstub stub-less uart? 

Simplest: use esptool run_stub + esp tool memread. We read SPI2 MMIO through the
RAM read command of the ROM loader stub (esp = esptool.cmds.ESP32S3ROM with stub).
Reads:
  SPI2 base 0x60025000: SPI_CMD(0x00), SPI_SLAVE(0x08? on S3 SPI_SLAVE=0x08?), clock reg SPI_CLOCK(0x14? -> 0x60025014 per pool constant at 0x42040324!)
We saw pool constants: SPI cmd 0x60025000, 0x60025010, 0x6002501c, FIFO 0x60025098.
On ESP32-S3 GPSPI2: SPI_SLAVE_REG=0x08? Actually S3: SPI_CMD 0x00, SPI_ADDR 0x04, SPI_CTRL 0x08, SPI_CLOCK 0x0C...
This build wrote SPI_CLOCK at +0x14? (0x60025014 was written via 0x42040324 pool). Let's just dump 0x60025000..0x100 and GPIO matrix regs for pins.
Also dump IO_MUX regs 0x60009000..0x60009070 and GPIO matrix 0x60009500.. to enumerate pins.
"""
import sys, time
import esptool

PORT = "COM8"

def main():
    # Use esptool's stub loader to read memory
    import esptool.cmds as cmds
    from esptool.loader import ESPLoader
    esp = cmds.detect_chip(PORT, 115200)
    print("Chip:", esp.CHIP_NAME, "stub:", esp.stub_running if hasattr(esp,'stub_running') else '?')
    try:
        esp = esp.run_stub()
        print("stub loaded")

        def rd(addr, words=1):
            return esp.read_reg(addr) if words==1 else None

        def dump_block(base, length, label):
            print(f"== {label} @ {base:#x} ==")
            for a in range(base, base+length, 4):
                try:
                    v = esp.read_reg(a)
                except Exception as e:
                    print(f"  {a:#09x}: ERR {e}")
                    continue
                if v != 0:
                    print(f"  {a:#09x}: {v:#010x}")

        # IO_MUX: 0x60009000 + 4*n for GPIO0..21 (S3 has IO_MUX for GPIO0..21 at 0x60009000..0x60009054,
        # then 26..32 at 0x6000905c..0x60009078, 33..37 at 0x6000907c.. etc, 38..48 higher)
        dump_block(0x60009000, 0x80, "IO_MUX (pins 0..31)")
        dump_block(0x60009078, 0x50, "IO_MUX (pins 32..51)")
        # GPIO matrix output selectors: GPIO_FUNC0_OUT_SEL_CFG = 0x60009540 + 4*n (S3: 0x60009540?)
        dump_block(0x60009540, 0x80*4, "GPIO matrix OUT sel 0..127")  # too big; limit
        # SPI2 header
        dump_block(0x60025000, 0x40, "SPI2 GPSPI2 regs")
    finally:
        try:
            esp._port.close()
        except Exception:
            pass

if __name__ == "__main__":
    main()
