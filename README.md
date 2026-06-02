# KryOS Linux Kernel Driver

High-performance SPI kernel driver for the KryOS Edge Gateway (Raspberry Pi 4).

## Features
- **Interrupt Driven**: Captures precision timestamps in the hard-IRQ context.
- **Workqueue Processing**: SPI transfers are deferred to process context for DMA safety.
- **SPSC Ring Buffer**: Lock-free single-producer single-consumer buffer for zero-jitter collection.
- **Security**: Hardware-accelerated HMAC-SHA256 re-verification in the kernel.
- **Sysfs Interface**: Exposes live telemetry via JSON for easy user-space integration.

## Installation

### 1. Build
```sh
make
```

### 2. Load Driver
```sh
sudo insmod kryos_spi.ko
```

### 3. Apply Device Tree Overlay
The driver requires the `rohit,kryos-root-node` compatible string.
```sh
cd pi-dts
dtc -@ -I dts -O dtb -o kryos-spi.dtbo kryos-spi.dts
sudo dtoverlay kryos-spi.dtbo
```

## Usage

### Live Telemetry
Read the current consensus state directly from sysfs:
```sh
cat /sys/class/kryos/kryos_device/telemetry
```
**Output Example:**
`{"round_id":158, "timestamp":360, "temp_raw":"0x41c80000 BIT-FLOAT", "node_mask":"0x0f", "status_flags":"0x00"}`

### Diagnostics
Check for dropped packets or authentication failures:
```sh
cat /sys/class/kryos/kryos_device/dropped
dmesg | grep KryOS
```

## Security
The driver uses a 32-byte `MASTER_PSK` hardcoded in `kryos_spi.c`. This key **must** match the key in the ESP32 firmware for data to be accepted.
