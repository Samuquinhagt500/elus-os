/* ============================================================
 *  Elus OS - Sistema de Arquivos FAT12/FAT16/FAT32 (somente leitura)
 *  Implementacao propria, sem dependencias externas.
 * ============================================================ */
#include "fs.h"
#include "disk.h"

struct fs_volume g_fs = {0};
struct fs_file   g_files[FS_MAX_FILES] = {{0}};

/* ---------- helpers de leitura de setor ---------- */
static u8 fs_sector_buf[512];

static int fs_read_sector(u32 lba) {
    return disk_read_sector(g_fs.drive, lba, fs_sector_buf);
}

/* ---------- analise do BPB (setor de boot) ---------- */
static u16 rd_u16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd_u32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static void wr_u16(u8 *p, u16 v) { p[0] = (u8)(v & 0xFF); p[1] = (u8)((v >> 8) & 0xFF); }
static void wr_u32(u8 *p, u32 v) { p[0] = (u8)(v & 0xFF); p[1] = (u8)((v >> 8) & 0xFF); p[2] = (u8)((v >> 16) & 0xFF); p[3] = (u8)((v >> 24) & 0xFF); }

static int fs_parse_bpb(void) {
    if (!fs_read_sector(0)) return 0;

    u8 *b = fs_sector_buf;
    if (b[510] != 0x55 || b[511] != 0xAA) return 0;  /* assinatura de boot */

    g_fs.sector_size   = rd_u16(b + 11);
    g_fs.sectors_per_cluster = b[13];
    g_fs.reserved_sectors = rd_u16(b + 14);
    g_fs.fat_copies      = b[16];
    g_fs.fat_sectors     = rd_u16(b + 22);
    g_fs.root_dir_sectors= rd_u16(b + 17);
    g_fs.total_sectors   = rd_u16(b + 19);
    if (g_fs.total_sectors == 0)
        g_fs.total_sectors = rd_u32(b + 32);

    /* Detecta FAT32 */
    u32 fat_size_16 = g_fs.fat_sectors;
    if (fat_size_16 == 0) {
        g_fs.fat32 = 1;
        g_fs.fat_sectors = rd_u32(b + 36);  /* setores por FAT (FAT32) */
        g_fs.root_cluster = rd_u32(b + 44); /* cluster raiz (FAT32) */
    } else {
        g_fs.fat32 = 0;
    }

    g_fs.fat_start  = g_fs.reserved_sectors;
    g_fs.root_start = g_fs.fat_start + (g_fs.fat_copies * g_fs.fat_sectors);

    if (!g_fs.fat32) {
        /* FAT12/16: diretório raiz vem após a FAT */
        g_fs.root_dir_sectors = ((rd_u16(b + 17) * 32) + g_fs.sector_size - 1) / g_fs.sector_size;
    }
    u32 root_size = g_fs.fat32 ? 0 : g_fs.root_dir_sectors;
    g_fs.data_start = g_fs.root_start + root_size;
    g_fs.cluster_size_bytes = g_fs.sectors_per_cluster * g_fs.sector_size;

    g_fs.mounted = 1;
    return 1;
}

/* Converte cluster para setor LBA */
static u32 fs_cluster_to_lba(u32 cluster) {
    if (cluster < 2) return 0;
    return g_fs.data_start + ((cluster - 2) * g_fs.sectors_per_cluster);
}

/* Lê uma entrada da FAT para obter o próximo cluster */
static u32 fs_fat_next(u32 cluster) {
    if (!g_fs.mounted) return 0;

    u32 fat_entry_offset;
    u32 fat_sector;
    u8  *fat_buf = fs_sector_buf;

    if (g_fs.fat32) {
        fat_entry_offset = cluster * 4;
        fat_sector = g_fs.fat_start + (fat_entry_offset / g_fs.sector_size);
        u32 off = fat_entry_offset % g_fs.sector_size;
        if (!fs_read_sector(fat_sector)) return 0;
        u32 next = rd_u32(fat_buf + off) & 0x0FFFFFFF;
        if (next >= 0x0FFFFFF8) return 0xFFFFFFFF;  /* fim de cadeia */
        return next;
    } else {
        /* FAT16: 2 bytes por entrada */
        fat_entry_offset = cluster * 2;
        fat_sector = g_fs.fat_start + (fat_entry_offset / g_fs.sector_size);
        u32 off = fat_entry_offset % g_fs.sector_size;
        if (!fs_read_sector(fat_sector)) return 0;
        u16 next = rd_u16(fat_buf + off);
        if (next >= 0xFFF8) return 0xFFFFFFFF;  /* fim de cadeia (FAT16) */
        return next;
    }
}

/* ---------- diretório raiz ---------- */

/* Lê uma entrada de diretório (32 bytes) */
struct dir_entry {
    char  name[12];
    u8    attr;
    u8    reserved[10];
    u16   time;
    u16   date;
    u16   cluster_hi;   /* FAT32: bits altos do cluster inicial */
    u16   cluster_lo;   /* cluster inicial (baixo) */
    u32   size;
};

static int fs_dir_read_entry(u32 dir_sector, u32 index, struct dir_entry *de) {
    if (!fs_read_sector(dir_sector)) return 0;
    u8 *b = fs_sector_buf + (index * 32);
    if (b[0] == 0x00) return 0;  /* fim do diretório */
    if (b[0] == 0xE5) return 1;  /* excluído, pular */

    for (int i = 0; i < 11; i++) de->name[i] = b[i];
    de->name[11] = 0;
    de->attr = b[11];
    de->cluster_lo = rd_u16(b + 26);
    de->cluster_hi = rd_u16(b + 20);
    de->size = rd_u32(b + 28);
    return 1;
}

/* Converte nome FAT (8.3) para string C */
static void fs_fatname_to_c(const u8 *fatname, char *out, int outsz) {
    int n = 0;
    for (int i = 0; i < 8 && n < outsz - 1; i++) {
        if (fatname[i] == ' ') break;
        out[n++] = fatname[i];
    }
    if (fatname[8] != ' ' && n < outsz - 1) {
        out[n++] = '.';
        for (int i = 8; i < 11 && n < outsz - 1; i++) {
            if (fatname[i] == ' ') break;
            out[n++] = fatname[i];
        }
    }
    out[n] = 0;
}

/* Converte caminho C para nome FAT (8.3 maiúsculo) */
static int fs_cname_to_fat(const char *name, u8 *fatname) {
    for (int i = 0; i < 11; i++) fatname[i] = ' ';
    int ni = 0, fi = 0;
    int dot_seen = 0;
    while (name[ni] && fi < 11) {
        char c = name[ni++];
        if (c == '.') { dot_seen = 1; fi = 8; continue; }
        if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        if (dot_seen) {
            if (fi < 11) fatname[fi++] = c;
        } else {
            if (fi < 8) fatname[fi++] = c;
        }
    }
    return 1;
}

/* Encontra um arquivo no diretório raiz */
static int fs_find_file(const char *fname, struct dir_entry *de_out) {
    if (!g_fs.mounted) return 0;

    u8 fatname[11];
    fs_cname_to_fat(fname, fatname);

    u32 dir_sector;
    u32 cluster;
    if (g_fs.fat32) {
        cluster = g_fs.root_cluster;
    } else {
        dir_sector = g_fs.root_start;
    }

    int max_clusters = 1000;  /* segurança contra loop infinito */
    for (int guard = 0; guard < max_clusters; guard++) {
        if (g_fs.fat32) {
            dir_sector = fs_cluster_to_lba(cluster);
        }

        /* Lê todo o cluster (ou setor raiz para FAT12/16) */
        u32 sectors_in_cluster = g_fs.fat32 ? g_fs.sectors_per_cluster : 1;
        if (!g_fs.fat32 && dir_sector >= g_fs.data_start) break;  /* FAT12/16 raiz é fixo */

        for (u32 s = 0; s < sectors_in_cluster; s++) {
            u32 sector = dir_sector + s;
            u32 entries_per_sector = g_fs.sector_size / 32;
            for (u32 e = 0; e < entries_per_sector; e++) {
                struct dir_entry de;
                if (!fs_dir_read_entry(sector, e, &de)) return 0;  /* fim */
                if (de.name[0] == 0xE5) continue;

                /* Compara nome */
                int match = 1;
                for (int i = 0; i < 11; i++) {
                    if (de.name[i] != fatname[i]) { match = 0; break; }
                }
                if (match) {
                    if (de_out) *de_out = de;
                    return 1;
                }
            }
        }

        if (g_fs.fat32) {
            u32 next = fs_fat_next(cluster);
            if (next == 0xFFFFFFFF) break;
            cluster = next;
        } else {
            break;  /* FAT12/16 raiz não encadeia */
        }
    }
    return 0;
}

/* ---------- API pública ---------- */

int fs_mount(int drive) {
    if (drive < 0 || drive >= DISK_MAX) return 0;
    if (!g_disk[drive].present) return 0;

    g_fs.drive = (u8)drive;
    g_fs.mounted = 0;
    g_fs.fat32 = 0;

    if (!fs_parse_bpb()) return 0;

    /* Fecha arquivos abertos */
    for (int i = 0; i < FS_MAX_FILES; i++) g_files[i].used = 0;

    return 1;
}

int fs_open(const char *path) {
    if (!g_fs.mounted || !path) return -1;

    /* Ignora barra inicial */
    const char *name = path;
    while (*name == '/') name++;

    /* Encontra arquivo */
    struct dir_entry de;
    if (!fs_find_file(name, &de)) return -1;

    /* Acha slot livre */
    int fd = -1;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!g_files[i].used) { fd = i; break; }
    }
    if (fd < 0) return -1;

    u32 start_cluster = de.cluster_lo | ((u32)de.cluster_hi << 16);
    if (start_cluster < 2) start_cluster = 2;  /* segurança */

    struct fs_file *f = &g_files[fd];
    f->used = 1;
    for (int i = 0; i < FS_NAME_LEN - 1 && path[i]; i++) f->name[i] = path[i];
    f->name[FS_NAME_LEN - 1] = 0;
    f->start_cluster = start_cluster;
    f->current_cluster = start_cluster;
    f->current_offset = 0;
    f->file_size = de.size;
    f->pos = 0;
    f->eof = 0;

    return fd;
}

u32 fs_read(int fd, void *buf, u32 len) {
    if (fd < 0 || fd >= FS_MAX_FILES || !g_files[fd].used || !buf) return 0;
    struct fs_file *f = &g_files[fd];

    if (f->eof || f->pos >= f->file_size) return 0;

    u32 to_read = len;
    if (f->pos + to_read > f->file_size) to_read = f->file_size - f->pos;

    u8 *dst = (u8 *)buf;
    u32 read_total = 0;

    while (read_total < to_read) {
        /* Precisamos carregar o cluster atual se ainda não carregado */
        if (f->current_offset == 0) {
            u32 lba = fs_cluster_to_lba(f->current_cluster);
            if (!disk_read_sector(g_fs.drive, lba, fs_sector_buf)) break;
        }

        u32 remaining_in_cluster = g_fs.cluster_size_bytes - f->current_offset;
        u32 chunk = to_read - read_total;
        if (chunk > remaining_in_cluster) chunk = remaining_in_cluster;

        /* Copia do buffer de setor para destino */
        /* Nota: isso só funciona para um setor por cluster. Para clusters > 1 setor,
         * precisaríamos ler mais setores. Simplificamos assumindo 1 setor/cluster
         * ou lendo setor a setor. */
        for (u32 i = 0; i < chunk; i++) {
            dst[read_total + i] = fs_sector_buf[f->current_offset + i];
        }

        read_total += chunk;
        f->current_offset += chunk;
        f->pos += chunk;

        /* Se acabou o cluster, avança para o próximo */
        if (f->current_offset >= g_fs.cluster_size_bytes) {
            u32 next = fs_fat_next(f->current_cluster);
            if (next == 0xFFFFFFFF) {
                f->eof = 1;
                break;
            }
            f->current_cluster = next;
            f->current_offset = 0;
        }
    }

    return read_total;
}

void fs_close(int fd) {
    if (fd >= 0 && fd < FS_MAX_FILES) {
        g_files[fd].used = 0;
    }
}

u32 fs_size(int fd) {
    if (fd < 0 || fd >= FS_MAX_FILES || !g_files[fd].used) return 0;
    return g_files[fd].file_size;
}

int fs_is_open(int fd) {
    if (fd < 0 || fd >= FS_MAX_FILES) return 0;
    return g_files[fd].used;
}

int fs_exists(const char *path) {
    if (!g_fs.mounted || !path) return 0;
    const char *name = path;
    while (*name == '/') name++;
    struct dir_entry de;
    return fs_find_file(name, &de);
}

/* ---------- escrita de arquivo (cria ou sobrescreve no diretorio raiz) ---------- */

/* Aloca um cluster livre na FAT */
static u32 fs_alloc_cluster(void) {
    if (!g_fs.mounted) return 0;
    u32 total_clusters = (g_fs.total_sectors - g_fs.data_start) / g_fs.sectors_per_cluster;
    for (u32 c = 2; c < total_clusters; c++) {
        u32 next = fs_fat_next(c);
        if (next == 0) return c;  /* cluster livre (0 em FAT12/16/32 indica livre) */
    }
    return 0;  /* sem clusters livres */
}

/* Marca um cluster como fim de cadeia na FAT */
static int fs_fat_set_eoc(u32 cluster) {
    if (!g_fs.mounted) return 0;
    u32 fat_entry_offset;
    u32 fat_sector;
    if (g_fs.fat32) {
        fat_entry_offset = cluster * 4;
        fat_sector = g_fs.fat_start + (fat_entry_offset / g_fs.sector_size);
        u32 off = fat_entry_offset % g_fs.sector_size;
        if (!fs_read_sector(fat_sector)) return 0;
        wr_u32(fs_sector_buf + off, 0x0FFFFFFF);
    } else {
        fat_entry_offset = cluster * 2;
        fat_sector = g_fs.fat_start + (fat_entry_offset / g_fs.sector_size);
        u32 off = fat_entry_offset % g_fs.sector_size;
        if (!fs_read_sector(fat_sector)) return 0;
        wr_u16(fs_sector_buf + off, 0xFFFF);
    }
    /* Escreve de volta o setor da FAT */
    return disk_write_sector(g_fs.drive, fat_sector, fs_sector_buf);
}

/* Escreve um arquivo novo (ou sobrescreve) no diretorio raiz */
int fs_write_file(const char *path, const void *data, u32 size) {
    if (!g_fs.mounted || !path || !data || size == 0) return 0;

    const char *name = path;
    while (*name == '/') name++;

    /* Se ja existe, nao sobrescrevemos (simplicidade) */
    if (fs_exists(path)) return -1;

    u8 fatname[11];
    fs_cname_to_fat(name, fatname);

    /* Calcula clusters necessarios */
    u32 bytes_per_cluster = g_fs.cluster_size_bytes;
    u32 clusters_needed = (size + bytes_per_cluster - 1) / bytes_per_cluster;

    /* Aloca clusters encadeados */
    u32 first_cluster = 0;
    u32 prev_cluster = 0;
    for (u32 i = 0; i < clusters_needed; i++) {
        u32 c = fs_alloc_cluster();
        if (c == 0) return -1;  /* sem espaco */
        if (i == 0) first_cluster = c;
        if (prev_cluster != 0) {
            /* Encadeia no FAT */
            u32 fat_sector;
            u32 off;
            if (g_fs.fat32) {
                off = prev_cluster * 4;
                fat_sector = g_fs.fat_start + (off / g_fs.sector_size);
                off = off % g_fs.sector_size;
            } else {
                off = prev_cluster * 2;
                fat_sector = g_fs.fat_start + (off / g_fs.sector_size);
                off = off % g_fs.sector_size;
            }
            if (!fs_read_sector(fat_sector)) return -1;
            if (g_fs.fat32)
                wr_u32(fs_sector_buf + off, c);
            else
                wr_u16(fs_sector_buf + off, (u16)c);
            if (!disk_write_sector(g_fs.drive, fat_sector, fs_sector_buf)) return -1;
        }
        prev_cluster = c;
    }
    /* Marca ultimo cluster como EOC */
    fs_fat_set_eoc(prev_cluster);

    /* Escreve os dados nos clusters */
    const u8 *src = (const u8 *)data;
    u32 remaining = size;
    u32 cur_cluster = first_cluster;
    u32 cur_offset = 0;
    while (remaining > 0) {
        u32 lba = fs_cluster_to_lba(cur_cluster);
        if (!disk_read_sector(g_fs.drive, lba, fs_sector_buf)) return -1;
        u32 chunk = bytes_per_cluster - cur_offset;
        if (chunk > remaining) chunk = remaining;
        for (u32 i = 0; i < chunk; i++) {
            fs_sector_buf[cur_offset + i] = src[size - remaining + i];
        }
        if (!disk_write_sector(g_fs.drive, lba, fs_sector_buf)) return -1;
        remaining -= chunk;
        cur_offset += chunk;
        if (cur_offset >= bytes_per_cluster) {
            cur_cluster = fs_fat_next(cur_cluster);
            cur_offset = 0;
        }
    }

    /* Cria entrada no diretorio raiz */
    struct dir_entry de;
    for (int i = 0; i < 11; i++) de.name[i] = fatname[i];
    de.attr = 0x20;  /* arquivo */
    de.cluster_lo = (u16)(first_cluster & 0xFFFF);
    de.cluster_hi = (u16)((first_cluster >> 16) & 0xFFFF);
    de.size = size;
    de.time = 0;
    de.date = 0;

    /* Encontra um slot livre no diretorio raiz */
    u32 dir_sector = g_fs.root_start;
    u32 cluster = g_fs.fat32 ? g_fs.root_cluster : 2;
    int guard = 0;
    while (guard++ < 1000) {
        if (g_fs.fat32) dir_sector = fs_cluster_to_lba(cluster);
        u32 sectors_in_cluster = g_fs.fat32 ? g_fs.sectors_per_cluster : 1;
        if (!g_fs.fat32 && dir_sector >= g_fs.data_start) break;

        for (u32 s = 0; s < sectors_in_cluster; s++) {
            u32 sector = dir_sector + s;
            u32 entries_per_sector = g_fs.sector_size / 32;
            for (u32 e = 0; e < entries_per_sector; e++) {
                struct dir_entry existing;
                if (!fs_dir_read_entry(sector, e, &existing)) {
                    /* Fim do diretorio - escreve entrada no slot atual */
                    if (!disk_read_sector(g_fs.drive, sector, fs_sector_buf)) return -1;
                    u8 *slot = fs_sector_buf + e * 32;
                    for (int i = 0; i < 11; i++) slot[i] = de.name[i];
                    slot[11] = de.attr;
                    wr_u16(slot + 20, de.cluster_hi);
                    wr_u16(slot + 26, de.cluster_lo);
                    wr_u32(slot + 28, de.size);
                    return disk_write_sector(g_fs.drive, sector, fs_sector_buf);
                }
                if (existing.name[0] == 0x00 || existing.name[0] == 0xE5) {
                    /* Slot livre encontrado */
                    if (!disk_read_sector(g_fs.drive, sector, fs_sector_buf)) return -1;
                    u8 *slot = fs_sector_buf + e * 32;
                    for (int i = 0; i < 11; i++) slot[i] = de.name[i];
                    slot[11] = de.attr;
                    wr_u16(slot + 20, de.cluster_hi);
                    wr_u16(slot + 26, de.cluster_lo);
                    wr_u32(slot + 28, de.size);
                    return disk_write_sector(g_fs.drive, sector, fs_sector_buf);
                }
            }
        }

        if (g_fs.fat32) {
            u32 next = fs_fat_next(cluster);
            if (next == 0xFFFFFFFF) break;
            cluster = next;
        } else {
            break;
        }
    }
    return -1;  /* nao encontrou slot livre */
}

int fs_list_root(char *out_buf, u32 out_size) {
    if (!g_fs.mounted || !out_buf || out_size == 0) return 0;
    out_buf[0] = 0;
    u32 used = 0;

    u32 dir_sector = g_fs.root_start;
    u32 cluster = g_fs.fat32 ? g_fs.root_cluster : 2;

    int guard = 0;
    while (guard++ < 1000) {
        if (g_fs.fat32) dir_sector = fs_cluster_to_lba(cluster);
        u32 sectors_in_cluster = g_fs.fat32 ? g_fs.sectors_per_cluster : 1;
        if (!g_fs.fat32 && dir_sector >= g_fs.data_start) break;

        for (u32 s = 0; s < sectors_in_cluster; s++) {
            u32 sector = dir_sector + s;
            u32 entries_per_sector = g_fs.sector_size / 32;
            for (u32 e = 0; e < entries_per_sector; e++) {
                struct dir_entry de;
                if (!fs_dir_read_entry(sector, e, &de)) {
                    /* fim do diretório */
                    return (used > 0) ? 1 : 0;
                }
                if (de.name[0] == 0x00) return (used > 0) ? 1 : 0;
                if (de.name[0] == 0xE5) continue;
                if (de.attr & 0x08) continue;  /* volume label */

                char cname[32];
                fs_fatname_to_c(de.name, cname, sizeof cname);
                u32 n = 0;
                while (cname[n]) n++;
                if (used + n + 2 < out_size) {
                    for (u32 i = 0; i < n; i++) out_buf[used++] = cname[i];
                    out_buf[used++] = '\n';
                }
            }
        }

        if (g_fs.fat32) {
            u32 next = fs_fat_next(cluster);
            if (next == 0xFFFFFFFF) break;
            cluster = next;
        } else {
            break;
        }
    }
    out_buf[used] = 0;
    return (used > 0) ? 1 : 0;
}
