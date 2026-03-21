# AirGradient-NAND-Storage Component

This component provides a HAL for SPI NAND flash storage, abstracting the
hardware lifecycle (SPI device, NAND flash driver, FATFS mount) behind a
clean interface.

Once `init()` succeeds the filesystem is available at the configured mount
path. All I/O above that point is plain POSIX (`fopen`, `fwrite`, `fread`,
`fseek`, `fsync`) with no ESP-IDF involvement. Record formats, file layouts,
FreeRTOS tasks, queues, and sync policies are application concerns and belong
in product-specific code.

## Responsibilities

- Register and release the SPI device on a caller-initialised bus
- Initialise and deinitialise the SPI NAND flash driver
- Mount and unmount the FATFS filesystem via `esp_vfs_fat_nand`
- Expose the mount path for POSIX I/O by application code

## Directory Layout

```text
components/airgradient-nand-storage/
  hal/
  drivers/
  CMakeLists.txt
  README.md
```

- `hal/` — `NandStorage` abstract interface
- `drivers/` — `SpiNandStorage` concrete driver with `Config` struct

## Design Direction

```text
product task -> NandStorage& -> SpiNandStorage -> spi_nand_flash -> NAND chip
                                              -> esp_vfs_fat_nand -> FATFS at mount_path()

product task <- POSIX file I/O on mount_path() (fopen / fwrite / fread / fsync)
```

Product composition code creates a `SpiNandStorage` instance and passes a
`NandStorage&` to any service or task that needs storage access. All POSIX
I/O happens directly on the mounted path; this component does not define any
record formats or file structures.

`spi_nand_flash` (ESP-IDF extra managed component) is a private dependency:
its headers are not exposed to callers of this component.

## Prerequisites

The SPI bus must be initialised with `spi_bus_initialize()` before calling
`init()`. This driver adds and removes its own device handle on the bus.

## Typical Usage

```cpp
// Product BSP / app_main wiring (firmware-only)
SpiNandStorage nand({
    .spi_host              = SPI2_HOST,
    .cs_pin                = GPIO_NUM_10,
    .mount_path            = "/nand",
    .format_if_mount_failed = true,
});

if (!nand.init()) {
    // handle mount failure
}

// Application task — pure POSIX on the mounted path
FILE *f = fopen("/nand/records.bin", "ab");
fwrite(&record, sizeof(record), 1, f);
fclose(f);

// Teardown
nand.deinit();
```
