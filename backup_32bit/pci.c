#include "pci.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static inline void outl(u16 p, u32 v) {
    __asm__ volatile("outl %0,%1" :: "a"(v), "Nd"(p));
}

static inline u32 inl(u16 p) {
    u32 r;
    __asm__ volatile("inl %1,%0" : "=a"(r) : "Nd"(p));
    return r;
}

static inline void outw(u16 p, u16 v) {
    __asm__ volatile("outw %0,%1" :: "a"(v), "Nd"(p));
}

static inline u16 inw(u16 p) {
    u16 r;
    __asm__ volatile("inw %1,%0" : "=a"(r) : "Nd"(p));
    return r;
}

static inline u8 inb(u16 p) {
    u8 r;
    __asm__ volatile("inb %1,%0" : "=a"(r) : "Nd"(p));
    return r;
}

u32 pci_read_dword(u8 bus, u8 slot, u8 func, u8 offset) {
    u32 addr = (1u << 31) | ((u32)bus << 16) | ((u32)slot << 11) | ((u32)func << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, addr);
    return inl(PCI_CONFIG_DATA);
}

u16 pci_read_word(u8 bus, u8 slot, u8 func, u8 offset) {
    u32 d = pci_read_dword(bus, slot, func, offset);
    return (u16)((d >> ((offset & 2) * 8)) & 0xFFFF);
}

u8 pci_read_byte(u8 bus, u8 slot, u8 func, u8 offset) {
    u32 d = pci_read_dword(bus, slot, func, offset);
    return (u8)((d >> ((offset & 3) * 8)) & 0xFF);
}

void pci_write_dword(u8 bus, u8 slot, u8 func, u8 offset, u32 val) {
    u32 addr = (1u << 31) | ((u32)bus << 16) | ((u32)slot << 11) | ((u32)func << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, addr);
    outl(PCI_CONFIG_DATA, val);
}

void pci_write_word(u8 bus, u8 slot, u8 func, u8 offset, u16 val) {
    u32 addr = (1u << 31) | ((u32)bus << 16) | ((u32)slot << 11) | ((u32)func << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, addr);
    outw(PCI_CONFIG_DATA + (offset & 2), val);
}

int pci_find_device(u16 vendor, u16 device, struct pci_dev *out_dev) {
    for (u16 bus = 0; bus < 256; bus++) {
        for (u8 slot = 0; slot < 32; slot++) {
            for (u8 func = 0; func < 8; func++) {
                u16 v = pci_read_word((u8)bus, slot, func, 0x00);
                if (v == 0xFFFF || v == 0x0000) {
                    if (func == 0) break; /* Nao ha funcoes neste slot */
                    continue;
                }
                u16 d = pci_read_word((u8)bus, slot, func, 0x02);
                if (v == vendor && d == device) {
                    if (out_dev) {
                        out_dev->bus = (u8)bus;
                        out_dev->slot = slot;
                        out_dev->func = func;
                        out_dev->vendor_id = v;
                        out_dev->device_id = d;
                        out_dev->bar0 = pci_read_dword((u8)bus, slot, func, 0x10);
                        out_dev->bar1 = pci_read_dword((u8)bus, slot, func, 0x14);
                        out_dev->irq  = pci_read_byte((u8)bus, slot, func, 0x3C);
                    }
                    return 1;
                }
            }
        }
    }
    return 0;
}

void pci_enable_bus_master(const struct pci_dev *dev) {
    if (!dev) return;
    u16 cmd = pci_read_word(dev->bus, dev->slot, dev->func, 0x04);
    /* Bit 0: I/O Space, Bit 1: Memory Space, Bit 2: Bus Master */
    cmd |= 0x0007;
    pci_write_word(dev->bus, dev->slot, dev->func, 0x04, cmd);
}
