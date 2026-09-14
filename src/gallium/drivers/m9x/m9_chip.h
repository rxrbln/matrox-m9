/* SPDX-License-Identifier: MIT */
#ifndef M9_CHIP_H
#define M9_CHIP_H

#include <stdbool.h>
#include <stdint.h>

static inline bool
m9_is_pci_device(uint16_t vendor_id, uint16_t device_id)
{
   return vendor_id == 0x102b && device_id == 0x0540;
}

#endif
