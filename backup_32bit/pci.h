#ifndef PCI_H
#define PCI_H

#include "sound.h"

struct pci_dev {
    u8 bus;
    u8 slot;
    u8 func;
    u16 vendor_id;
    u16 device_id;
    u32 bar0;
    u32 bar1;
    u8 irq;
};

/* Leitura e escrita na configuracao PCI (Portas 0xCF8 / 0xCFC) */
u32  pci_read_dword(u8 bus, u8 slot, u8 func, u8 offset);
u16  pci_read_word(u8 bus, u8 slot, u8 func, u8 offset);
u8   pci_read_byte(u8 bus, u8 slot, u8 func, u8 offset);
void pci_write_dword(u8 bus, u8 slot, u8 func, u8 offset, u32 val);
void pci_write_word(u8 bus, u8 slot, u8 func, u8 offset, u16 val);

/* Busca dispositivo por Vendor ID e Device ID */
int  pci_find_device(u16 vendor, u16 device, struct pci_dev *out_dev);

/* Ativa Bus Mastering, I/O Space e Memory Space */
void pci_enable_bus_master(const struct pci_dev *dev);

#endif
