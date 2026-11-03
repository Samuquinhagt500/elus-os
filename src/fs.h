#ifndef FS_H
#define FS_H

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

/* ---------- sistema de arquivos FAT (somente leitura) ---------- */

#define FS_MAX_FILES   8      /* arquivos abertos simultâneos */
#define FS_NAME_LEN    32     /* comprimento máximo do nome */

/* Estrutura de um arquivo aberto */
struct fs_file {
    int   used;               /* 1 se esta entrada esta em uso */
    char  name[FS_NAME_LEN];  /* nome do arquivo */
    u32   start_cluster;      /* cluster inicial */
    u32   current_cluster;    /* cluster atual durante leitura sequencial */
    u32   current_offset;     /* offset dentro do cluster atual */
    u32   file_size;          /* tamanho total em bytes */
    u32   pos;                /* posição atual de leitura */
    int   eof;                /* 1 se atingiu o final */
};

/* Informação do volume FAT */
struct fs_volume {
    int   mounted;
    u32   sector_size;        /* geralmente 512 */
    u32   sectors_per_cluster;
    u32   reserved_sectors;
    u32   fat_copies;
    u32   fat_sectors;
    u32   root_dir_sectors;   /* FAT12/16: setores do diretório raiz */
    u32   data_start;         /* setor onde começa a região de dados */
    u32   fat_start;          /* setor onde começa a FAT */
    u32   root_start;         /* setor onde começa o diretório raiz */
    u32   total_sectors;
    u32   cluster_size_bytes;
    int   fat32;              /* 1 se FAT32, 0 se FAT12/16 */
    u32   root_cluster;       /* FAT32: cluster inicial do diretório raiz */
    u8    drive;              /* número do disco IDE */
};

/* Estado global do sistema de arquivos */
extern struct fs_volume g_fs;
extern struct fs_file   g_files[FS_MAX_FILES];

/* Monta o sistema de arquivos de um disco */
int  fs_mount(int drive);

/* Abre um arquivo pelo caminho (ex: "MUSICA.MP3" ou "/MUSICA.MP3") */
int  fs_open(const char *path);

/* Lê bytes de um arquivo aberto */
u32  fs_read(int fd, void *buf, u32 len);

/* Fecha um arquivo */
void fs_close(int fd);

/* Retorna o tamanho de um arquivo aberto */
u32  fs_size(int fd);

/* Verifica se um arquivo está aberto */
int  fs_is_open(int fd);

/* Lista arquivos do diretório raiz (imprime nomes via debug ou retorno) */
int  fs_list_root(char *out_buf, u32 out_size);

/* Verifica se um arquivo existe */
int  fs_exists(const char *path);

/* ---------- escrita ---------- */
int  fs_write_file(const char *path, const void *data, u32 size);  /* cria/sobrescreve arquivo no diretorio raiz */

#endif
