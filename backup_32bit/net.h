#ifndef NET_H
#define NET_H

#include "pci.h"

#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP  0x0806

#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

struct net_info {
    int present;           /* 1 se a placa de rede RTL8139 foi encontrada */
    u16 io_base;           /* Endereco base de E/S (BAR0) */
    u8 irq;                /* Linha de interrupcao IRQ da placa */
    u8 mac[6];             /* Endereco fisico MAC */
    u32 ip;                /* Endereco IPv4 local (ex: 10.0.2.15) */
    u32 gateway;           /* Roteador padrao / Gateway (ex: 10.0.2.2) */
    u32 mask;              /* Mascara de sub-rede (255.255.255.0) */
    u32 dns;               /* Servidor DNS (ex: 10.0.2.3) */
    u32 packets_tx;        /* Total de pacotes transmitidos */
    u32 packets_rx;        /* Total de pacotes recebidos */
    u32 bytes_tx;          /* Total de bytes transmitidos */
    u32 bytes_rx;          /* Total de bytes recebidos */
    int link_up;           /* 1 se o cabo de rede estiver conectado */
    int last_ping_ms;      /* Tempo de resposta do ultimo ping em ms */
    int pings_sent;
    int pings_received;
};

/* Inicializacao e controle de rede */
void net_init(void);
void net_poll(void);               /* Processa pacotes recebidos na fila */
void net_irq_handler(void);        /* Handler de interrupcao do hardware */
void net_tick(void);               /* Chamado a cada 10 ms pelo PIT (IRQ0) */
u32  net_time_ms(void);            /* Tempo interno da pilha, em milissegundos */
const struct net_info *net_get_info(void);

/* Transmissao de pacotes */
int net_send_raw(const void *frame, u16 len);
int net_send_ipv4(u32 dest_ip, u8 protocol, const void *payload, u16 len);
int net_ping(u32 target_ip);       /* Envia ICMP Echo Request */

/* ---- DNS ---- */
int  net_dns_start(const char *host);   /* inicia consulta A (nao-bloqueante) */
int  net_dns_poll(void);                /* 1 = resolvido, -1 = erro, 0 = aguardando */
u32  net_dns_ip(void);                  /* IPv4 resolvido */

/* ---- HTTP ---- */
#define HTTP_ERR_NONE      0
#define HTTP_ERR_URL      -1
#define HTTP_ERR_HTTPS    -2
#define HTTP_ERR_DNS      -3
#define HTTP_ERR_CONNECT  -4
#define HTTP_ERR_TIMEOUT  -5
#define HTTP_ERR_TOO_BIG  -6
#define HTTP_ERR_PROTOCOL -7
#define HTTP_ERR_ABORTED  -9

#define HTTP_ST_IDLE    0
#define HTTP_ST_DNS     1
#define HTTP_ST_CONNECT 2
#define HTTP_ST_RECV    3
#define HTTP_ST_DONE    4
#define HTTP_ST_ERROR   5

struct http_info {
    int state;                  /* HTTP_ST_* */
    int error;                  /* HTTP_ERR_* */
    int status;                 /* codigo HTTP (200, 404, ...) */
    int has_length;             /* Content-Length presente */
    int chunked;                /* Transfer-Encoding: chunked */
    int truncated;              /* buffer cheio: pagina cortada */
    u32 ip;                     /* IP do servidor */
    u16 port;                   /* porta (80 por padrao) */
    u32 received;               /* bytes crus recebidos */
    u32 content_length;         /* valor de Content-Length */
    u32 body_off;               /* inicio do corpo no buffer */
    u32 body_len;               /* tamanho do corpo (pos-dechunk) */
    u8 *buf;                    /* buffer de recepcao */
    char host[128];
    char path[256];
    char url[384];
    char content_type[64];
    char location[256];
    char title[128];
};

int  http_get(const char *url, u8 *buf, u32 cap);
int  http_resolve(const char *base, const char *rel, char *out, int outsz);
void http_poll(void);                   /* chamar no loop principal */
void http_abort(void);
int  http_is_busy(void);
const struct http_info *http_get_info(void);

/* Utilitarios de formatacao de strings */
void net_format_mac(char *out);
void net_format_ip(u32 ip, char *out);

/* Callback para futuros aplicativos receberem pacotes de rede */
typedef void (*net_rx_callback_t)(const u8 *data, u16 len);
void net_set_rx_callback(net_rx_callback_t cb);

#ifdef NET_SELFTEST
void net_selftest(void);
#endif

#endif
