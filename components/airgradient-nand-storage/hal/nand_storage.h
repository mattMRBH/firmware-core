/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef NAND_STORAGE_H
#define NAND_STORAGE_H

// Abstract interface for a NAND-flash-backed filesystem mount.
//
// Implementations are responsible for the full hardware lifecycle:
// SPI device registration, NAND flash initialisation, and FATFS mount/unmount.
// Once init() succeeds, the filesystem is available for POSIX I/O at
// mount_path().
//
// ISR-safe: no
// Thread-safe: no
// Blocking: init() and deinit() block during hardware and filesystem operations
// Allocates: yes (init() allocates NAND driver state and FATFS buffers)
class NandStorage {
public:
  virtual ~NandStorage() = default;

  // Registers the SPI device, initialises the NAND flash, and mounts the
  // FATFS filesystem at the configured mount path. Blocking.
  // Returns false on any failure; leaves no resources allocated on failure.
  // Calling init() on an already-mounted instance is a no-op and returns true.
  virtual bool init() = 0;

  // Unmounts the FATFS filesystem, deinitialises the NAND flash, and removes
  // the SPI device. Blocking. Safe to call when not mounted or after a failed
  // init().
  virtual void deinit() = 0;

  // Returns true if the filesystem is currently mounted and ready for I/O.
  virtual bool is_mounted() const = 0;

  // Returns the VFS mount path (e.g. "/nand"). Only valid while is_mounted()
  // is true. The pointer remains valid for the lifetime of this object.
  virtual const char *mount_path() const = 0;
};

#endif // NAND_STORAGE_H
