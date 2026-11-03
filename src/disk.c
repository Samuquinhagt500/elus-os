/* ============================================================
 *  Elus OS - Driver de Disco IDE/ATA (modo PIO)
 *  Leitura/escrita de setores via portas de E/S 0x1F0-0x1F7.
 *  Sem drivers de terceiros: acesso direto ao hardware.
 * ============================================================ */
#include "disk.h"

/* Endereço do registrador LBA3 (drive/head) para um drive */
#define IO_LBA3(d) (IDE_LBA3 + (d * 0x20))

struct disk_info g_disk[DISK_MAX];

static inline void io_wait(void) {
    /* Leitura da porta de status alternativo gera um pequeno atraso */
    __asm__ volatile("inb $0x3F6" ::: "memory");
}

static inline u8 inb_port(u16 p) {
    u8 r;
    __asm__ volatile("inb %1,%0" : "=a"(r) : "Nd"(p));
    return r;
}
static inline void outb_port(u16 p, u8 v) {
    __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p));
}
static inline u16 inw_port(u16 p) {
    u16 r;
    __asm__ volatile("inw %1,%0" : "=a"(r) : "Nd"(p));
    return r;
}
static inline void outw_port(u16 p, u16 v) {
    __asm__ volatile("outw %0,%1" :: "a"(v), "Nd"(p));
}
static inline void insw_port(u16 p, void *buf, u32 nwords) {
    __asm__ volatile("rep insw" : "+D"(buf), "+c"(nwords) : "d"(p) : "memory");
}

/* Espera o disco ficar pronto (limpa BSY, espera DRQ se necessário) */
static int disk_wait(int drive, int wait_drq) {
    u16 alt = 0x3F6 + (drive * 0x20);

    int t;
    for (t = 0; t < 1000000; t++) {
        u8 st = inb_port(alt);
        if (!(st & STS_BSY)) break;
        io_wait();
    }
    if (t >= 1000000) return 0;  /* timeout */

    if (wait_drq) {
        for (t = 0; t < 1000000; t++) {
            u8 st = inb_port(alt);
            if (st & STS_DRQ) return 1;
            if (st & STS_BSY) break;
            io_wait();
        }
        return 0;
    }
    return 1;
}

/* Envia comando IDENTIFY e preenche a estrutura de informação */
static int disk_identify(int drive) {
    if (!disk_wait(drive, 0)) return 0;

    /* Seleciona o drive (0 = master, 1 = slave) */
    outb_port(IO_LBA3(drive), 0xA0 | (drive << 4));
    io_wait();
    io_wait();
    io_wait();
    io_wait();

    if (!disk_wait(drive, 0)) return 0;

    outb_port(IDE_SECCNT, 0);
    outb_port(IDE_LBA0, 0);
    outb_port(IDE_LBA1, 0);
    outb_port(IDE_LBA2, 0);
    outb_port(IDE_COMMAND, CMD_IDENTIFY);

    /* Espera pelo resultado (pode gerar IRQ, mas estamos sem handler de disco ainda) */
    u8 st = inb_port(IDE_STATUS);
    if (st == 0xFF) return 0;  /* nenhum dispositivo */

    if (!disk_wait(drive, 1)) return 0;

    u16 buf[256];
    insw_port(IDE_DATA, buf, 256);

    /* Verifica se é ATAPI */
    u16 gen_config = buf[0];
    if ((gen_config & 0x1F00) == 0x1400 || (gen_config & 0x1F00) == 0x8400) {
        g_disk[drive].is_atapi = 1;
        return 0;  /* não suportamos ATAPI ainda */
    }

    g_disk[drive].present = 1;
    g_disk[drive].is_atapi = 0;

    /* Modelo: palavras 27..46, cada uma com 2 bytes, trocados */
    char *model = (char *)g_disk[drive].model;
    for (int i = 0; i < 20; i++) {
        u16 w = buf[27 + i];
        model[i * 2]     = (u8)(w >> 8);
        model[i * 2 + 1] = (u8)(w & 0xFF);
    }
    model[40] = 0;
    /* Remove espaços finais */
    int mlen = 40;
    while (mlen > 0 && model[mlen - 1] == ' ') mlen--;
    model[mlen] = 0;

    /* Total de setores LBA28: palavras 60-61 */
    u32 sectors = (u32)buf[61] << 16 | buf[60];
    g_disk[drive].sectors = sectors;
    g_disk[drive].size_mb = sectors / 2048;  /* aprox */

    return 1;
}

/* Endereço do registrador LBA3 (drive/head) para um drive */
#define IO_LBA3(d) (IDE_LBA3 + (d * 0x20))

void disk_init(void) {
    for (int d = 0; d < DISK_MAX; d++) {
        g_disk[d].present = 0;
        g_disk[d].is_atapi = 0;
        g_disk[d].sectors = 0;
        g_disk[d].size_mb = 0;
        g_disk[d].model[0] = 0;
        g_disk[d].lba48 = 0;
    }

    /* Detecta master e slave do canal primário */
    disk_identify(0);
    disk_identify(1);
}

int disk_read_sector(int drive, u32 lba, void *buf) {
    return disk_read_sectors(drive, lba, 1, buf);
}

int disk_read_sectors(int drive, u32 lba, u32 count, void *buf) {
    if (!g_disk[drive].present || !buf || count == 0) return 0;
    if (lba + count > g_disk[drive].sectors) return 0;

    u16 *p = (u16 *)buf;

    if (!disk_wait(drive, 0)) return 0;

    /* Seleciona drive e head (LBA28: bits 24-27 de LBA vão no drive/head) */
    u8 dh = 0xE0 | (drive << 4) | ((u8)(lba >> 24) & 0x0F);
    outb_port(IO_LBA3(drive), dh);
    io_wait();

    outb_port(IDE_SECCNT, (u8)count);
    outb_port(IDE_LBA0, (u8)(lba & 0xFF));
    outb_port(IDE_LBA1, (u8)((lba >> 8) & 0xFF));
    outb_port(IDE_LBA2, (u8)((lba >> 16) & 0xFF));
    outb_port(IDE_COMMAND, CMD_READ_PIO);

    for (u32 i = 0; i < count; i++) {
        if (!disk_wait(drive, 1)) return 0;
        insw_port(IDE_DATA, p, 256);
        p += 256;
    }

    /* Espera finalizar */
    return disk_wait(drive, 0);
}

int disk_write_sector(int drive, u32 lba, const void *buf) {
    if (!g_disk[drive].present || !buf) return 0;
    if (lba >= g_disk[drive].sectors) return 0;

    u16 *p = (u16 *)buf;

    if (!disk_wait(drive, 0)) return 0;

    /* Seleciona drive e head (LBA28) */
    u8 dh = 0xE0 | (drive << 4) | ((u8)(lba >> 24) & 0x0F);
    outb_port(IO_LBA3(drive), dh);
    io_wait();

    outb_port(IDE_SECCNT, 1);
    outb_port(IDE_LBA0, (u8)(lba & 0xFF));
    outb_port(IDE_LBA1, (u8)((lba >> 8) & 0xFF));
    outb_port(IDE_LBA2, (u8)((lba >> 16) & 0xFF));
    outb_port(IDE_COMMAND, CMD_WRITE_PIO);

    for (u32 i = 0; i < 256; i++) {
        if (!disk_wait(drive, 1)) return 0;
        outw_port(IDE_DATA, p[i]);
    }

    /* Espera finalizar a escrita */
    return disk_wait(drive, 0);
}

const struct disk_info *disk_get_info(int drive) {
    if (drive < 0 || drive >= DISK_MAX) return 0;
    return &g_disk[drive];
}

int disk_is_ready(int drive) {
    if (drive < 0 || drive >= DISK_MAX) return 0;
    u16 alt = 0x3F6 + (drive * 0x20);
    return !(inb_port(alt) & STS_BSY);
}
