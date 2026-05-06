#ifndef TEST_NAND_STORAGE_H
#define TEST_NAND_STORAGE_H

// Exercises SpiNandStorage: initialises the SPI bus, mounts FATFS, writes a
// test file, reads it back, and verifies the content.
//
// Requires SPI NAND flash hardware. Configure PIN_NAND_* in board_config.h.
// The test exits early and logs a warning if any PIN_NAND_* is GPIO_NUM_NC.
void run_test_nand_storage();

#endif // TEST_NAND_STORAGE_H
