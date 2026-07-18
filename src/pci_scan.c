/*
 * pci_scan.c - Enumerate PCI functions through Linux sysfs.
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pci_scan.h"

#define PCI_SYSFS_DEVICES "/sys/bus/pci/devices"

static int read_number(const char *path, unsigned int *value)
{
    FILE *file = fopen(path, "r");
    char text[64];
    char *end;
    unsigned long parsed;

    if (!file)
        return -errno;
    if (!fgets(text, sizeof(text), file)) {
        int error = ferror(file) ? -errno : -EINVAL;
        fclose(file);
        return error;
    }
    fclose(file);

    errno = 0;
    parsed = strtoul(text, &end, 0);
    if (errno || end == text)
        return errno ? -errno : -EINVAL;

    *value = (unsigned int)parsed;
    return 0;
}

static int id_matches(unsigned int candidate,
                      const uint16_t *device_ids,
                      size_t device_id_count)
{
    size_t i;

    for (i = 0; i < device_id_count; i++) {
        if (candidate == device_ids[i])
            return 1;
    }
    return 0;
}

static void read_resources(const char *bdf, struct l10gl_pci_device *device)
{
    char path[512];
    FILE *file;
    int i;

    snprintf(path, sizeof(path), PCI_SYSFS_DEVICES "/%s/resource", bdf);
    file = fopen(path, "r");
    if (!file)
        return;

    for (i = 0; i < 6; i++) {
        unsigned long long start, end, flags;

        if (fscanf(file, "%llx %llx %llx", &start, &end, &flags) != 3)
            break;
        if (end >= start && start <= UINT32_MAX) {
            unsigned long long size = end - start + 1;
            device->bar[i] = (uint32_t)start;
            device->bar_size[i] = size <= UINT32_MAX ? (uint32_t)size : 0;
        }
    }
    fclose(file);
}

int l10gl_pci_find(struct l10gl_pci_device *device,
                   uint16_t vendor,
                   const uint16_t *device_ids,
                   size_t device_id_count)
{
    const char *forced_bdf = getenv("L10GL_PCI_DEVICE");
    struct dirent **entries;
    int entry_count;
    int result = -ENODEV;
    int i;

    if (!device || !device_ids || device_id_count == 0)
        return -EINVAL;

    memset(device, 0, sizeof(*device));

    entry_count = scandir(PCI_SYSFS_DEVICES, &entries, NULL, alphasort);
    if (entry_count < 0)
        return -errno;

    for (i = 0; i < entry_count; i++) {
        const char *bdf = entries[i]->d_name;
        char path[512];
        unsigned int found_vendor, found_device;
        unsigned int domain, bus, dev, func;
        unsigned int irq;

        if (bdf[0] == '.')
            continue;
        if (forced_bdf && forced_bdf[0] && strcmp(bdf, forced_bdf) != 0)
            continue;

        snprintf(path, sizeof(path), PCI_SYSFS_DEVICES "/%s/vendor", bdf);
        if (read_number(path, &found_vendor) < 0 || found_vendor != vendor)
            continue;

        snprintf(path, sizeof(path), PCI_SYSFS_DEVICES "/%s/device", bdf);
        if (read_number(path, &found_device) < 0 ||
            !id_matches(found_device, device_ids, device_id_count))
            continue;

        if (sscanf(bdf, "%x:%x:%x.%x", &domain, &bus, &dev, &func) != 4)
            continue;

        device->domain = (int)domain;
        device->bus = (int)bus;
        device->dev = (int)dev;
        device->func = (int)func;
        device->device_id = (uint16_t)found_device;
        read_resources(bdf, device);

        snprintf(path, sizeof(path), PCI_SYSFS_DEVICES "/%s/irq", bdf);
        if (read_number(path, &irq) == 0)
            device->irq = (int)irq;

        result = 0;
        break;
    }

    for (i = 0; i < entry_count; i++)
        free(entries[i]);
    free(entries);
    return result;
}
