#ifndef DISK_H
#define DISK_H

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

/* ---------- driver de disco IDE/ATA (modo PIO) ---------- */

/* Discos suportados */
#define DISK_MAX 2

/* Registradores IDE (canal primário) */
#define IDE_DATA     0x1F0
#define IDE_ERROR    0x1F1
#define IDE_FEATURE  0x1F1
#define IDE_SECCNT   0x1F2
#define IDE_LBA0     0x1F3
#define IDE_LBA1     0x1F4
#define IDE_LBA2     0x1F5
#define IDE_LBA3     0x1F6
#define IDE_COMMAND  0x1F7
#define IDE_STATUS   0x1F7
#define IDE_ALTSTAT  0x3F6
#define IDE_CONTROL  0x3F6

/* Comandos ATA */
#define CMD_READ_PIO  0x20   /* Leitura setores (PIO) */
#define CMD_IDENTIFY  0xEC   /* Identificar dispositivo */
#define CMD_WRITE_PIO 0x30   /* Escrita setores (PIO) */

/* Status bits */
#define STS_BSY 0x80   /* Ocupado */
#define STS_DRDY 0x40  /* Pronto */
#define STS_DSC 0x20   /* Busca de conclusão */
#define STS_DRQ 0x08   /* Pronto para transferência de dados */

/* Estrutura de informação do disco */
struct disk_info {
    int present;        /* 1 se o disco foi detectado */
    int is_atapi;       /* 1 se for ATAPI (CD-ROM), 0 se ATA (HD) */
    u32  sectors;       /* Total de setores (LBA) */
    u32  size_mb;       /* Tamanho em MB (aproximado) */
    u16  model[21];     /* Nome do modelo (string, 40 chars) */
    int  lba48;         /* 1 se suporta LBA48 (discos > 128 GB) */
};

/* Estado global do disco */
extern struct disk_info g_disk[DISK_MAX];

/* Inicializa o driver de disco (detecta dispositivos IDE) */
void disk_init(void);

/* Lê um setor (512 bytes) de um disco para um buffer de memória */
int  disk_read_sector(int drive, u32 lba, void *buf);

/* Lê múltiplos setores contíguos */
int  disk_read_sectors(int drive, u32 lba, u32 count, void *buf);

/* Escreve um setor (512 bytes) de um buffer para o disco */
int  disk_write_sector(int drive, u32 lba, const void *buf);

/* Retorna informação do disco */
const struct disk_info *disk_get_info(int drive);

/* Verifica se o disco está pronto (não ocupado) */
int  disk_is_ready(int drive);

#endif
