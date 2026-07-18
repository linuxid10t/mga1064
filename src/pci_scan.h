/*
 * pci_scan.h - Small shared PCI sysfs discovery helper for L10GL backends.
 */

#ifndef L10GL_PCI_SCAN_H
#define L10GL_PCI_SCAN_H

#include <stddef.h>
#include <stdint.h>

struct l10gl_pci_device {
    int domain;
    int bus;
    int dev;
    int func;
    uint32_t bar[6];
    uint32_t bar_size[6];
    int irq;
    uint16_t device_id;
};

/* Find the first PCI function matching vendor and any ID in device_ids.
 * Returns 0 on success or a negative errno value. */
int l10gl_pci_find(struct l10gl_pci_device *device,
                   uint16_t vendor,
                   const uint16_t *device_ids,
                   size_t device_id_count);

#endif /* L10GL_PCI_SCAN_H */
