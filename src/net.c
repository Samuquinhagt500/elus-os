/* ============================================================
 *  Elus OS - Driver de Rede Realtek RTL8139 (PCI) + Pilha Ethernet/IPv4/ARP/ICMP
 * ============================================================ */
#include "net.h"

static inline void outb(u16 p, u8 v) { __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline u8  inb(u16 p)         { u8 r; __asm__ volatile("inb %1,%0" : "=a"(r) : "Nd"(p)); return r; }
static inline void outw(u16 p, u16 v) { __asm__ volatile("outw %0,%1" :: "a"(v), "Nd"(p)); }
static inline u16 inw(u16 p)         { u16 r; __asm__ volatile("inw %1,%0" : "=a"(r) : "Nd"(p)); return r; }
static inline void outl(u16 p, u32 v) { __asm__ volatile("outl %0,%1" :: "a"(v), "Nd"(p)); }
static inline u32 inl(u16 p)         { u32 r; __asm__ volatile("inl %1,%0" : "=a"(r) : "Nd"(p)); return r; }
static inline void io_wait(void)     { outb(0x80, 0); }

/* Helpers de memoria */
static void net_memcpy(void *d, const void *s, u32 n) {
    u8 *dst = (u8 *)d;
    const u8 *src = (const u8 *)s;
    while (n--) *dst++ = *src++;
}

static void net_memset(void *d, u8 v, u32 n) {
    u8 *dst = (u8 *)d;
    while (n--) *dst++ = v;
}

/* Tempo interno da pilha (incrementado pelo PIT a cada 10 ms) */
static volatile u32 net_ms;

/* Depuracao opcional via porta 0xE9 (QEMU -debugcon) */
#ifdef NET_DEBUG
static void ndbg(const char *s) { while (*s) outb(0xE9, (u8)*s++); }
static void ndbg_u32(u32 v) {
    char b[12]; int i = 11; b[i] = 0;
    if (!v) b[--i] = '0';
    while (v) { b[--i] = (char)('0' + v % 10); v /= 10; }
    ndbg(b + i);
}
static void ndbg_hex(u32 v) {
    char b[11]; b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 8; i++) b[2 + i] = "0123456789ABCDEF"[(v >> (28 - 4 * i)) & 15];
    b[10] = 0; ndbg(b);
}
#else
#define ndbg(s)        ((void)0)
#define ndbg_u32(v)    ((void)0)
#define ndbg_hex(v)    ((void)0)
#endif

/* Estado da interface de rede */
static struct net_info g_net = {
    .present = 0,
    .io_base = 0,
    .irq = 0,
    .mac = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 },
    .ip = (10u << 24) | (0u << 16) | (2u << 8) | 15u,       /* 10.0.2.15 (QEMU default) */
    .gateway = (10u << 24) | (0u << 16) | (2u << 8) | 2u,   /* 10.0.2.2  (QEMU router)  */
    .mask = (255u << 24) | (255u << 16) | (255u << 8) | 0u, /* 255.255.255.0            */
    .dns = (10u << 24) | (0u << 16) | (2u << 8) | 3u,       /* 10.0.2.3  (QEMU DNS)     */
    .packets_tx = 0,
    .packets_rx = 0,
    .bytes_tx = 0,
    .bytes_rx = 0,
    .link_up = 0,
    .last_ping_ms = -1,
    .pings_sent = 0,
    .pings_received = 0
};

/* Buffer circular de RX em endereco fisico seguro (0x210000, 12 KB) */
#define RX_BUF_ADDR 0x210000
#define RX_BUF_SIZE (8192 + 16 + 2048)
static u8 *const rx_buffer = (u8 *)RX_BUF_ADDR;
static u16 rx_offset = 0;

/* 4 Buffers de TX em 0x214000, 0x214800, 0x215000, 0x215800 (2 KB cada) */
#define TX_BUF_BASE 0x214000
static u8 *const tx_buffers[4] = {
    (u8 *)(TX_BUF_BASE + 0x0000),
    (u8 *)(TX_BUF_BASE + 0x0800),
    (u8 *)(TX_BUF_BASE + 0x1000),
    (u8 *)(TX_BUF_BASE + 0x1800)
};
static int tx_cur = 0;

/* Callback de recepcao para apps */
static net_rx_callback_t app_rx_cb = 0;

/* Despachantes das camadas superiores (implementados mais abaixo) */
static void net_handle_udp(u32 src_ip, const u8 *pkt, u16 len);
static void net_handle_tcp(u32 src_ip, const u8 *pkt, u16 len);
static void net_tcp_tick(void);

/* Cache simples de ARP para o Gateway */
static u8 gateway_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static int gateway_mac_resolved = 0;
static u32 ping_send_time = 0;

/* ============================================================
 *  RTL8139: Controle de Hardware
 * ============================================================ */
#define REG_MAC0   0x00
#define REG_MAR0   0x08
#define REG_TSD0   0x10
#define REG_TSAD0  0x20
#define REG_RBSTART 0x30
#define REG_CMD    0x37
#define REG_CAPR   0x38
#define REG_CBR    0x3A
#define REG_IMR    0x3C
#define REG_ISR    0x3E
#define REG_TCR    0x40
#define REG_RCR    0x44
#define REG_CONFIG1 0x52

/* Envio de frame bruto pela placa de rede */
int net_send_raw(const void *frame, u16 len) {
    if (!g_net.present || len == 0 || len > 1536) return 0;

    int idx = tx_cur;
    u8 *buf = tx_buffers[idx];

    /* Espera o buffer anterior terminar o DMA (bit OWN = 0 no TSD) */
    for (int t = 0; t < 200000; t++) {
        u32 tsd = inl(g_net.io_base + REG_TSD0 + idx * 4);
        if (!(tsd & (1u << 13))) break;
    }

    net_memcpy(buf, frame, len);
    /* Pad para tamanho minimo de frame Ethernet (60 bytes sem FCS) */
    if (len < 60) {
        net_memset(buf + len, 0, 60 - len);
        len = 60;
    }

    /* Registra endereco no TSAD e comprimento no TSD (inicia o envio imediato via DMA) */
    outl(g_net.io_base + REG_TSAD0 + idx * 4, (u32)buf);
    outl(g_net.io_base + REG_TSD0 + idx * 4, (u32)len);

    tx_cur = (idx + 1) % 4;
    g_net.packets_tx++;
    g_net.bytes_tx += len;
    return 1;
}

/* ============================================================
 *  Formatacao de Pacotes Ethernet / ARP / IPv4 / ICMP
 * ============================================================ */
static u16 net_checksum(const void *data, u32 len) {
    const u16 *p = (const u16 *)data;
    u32 sum = 0;
    while (len > 1) {
        sum += *p++;
        len -= 2;
    }
    if (len == 1) {
        sum += *(const u8 *)p;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (u16)(~sum);
}

/* Resposta ou Envio de ARP */
static void net_send_arp(u16 opcode, const u8 *target_mac, u32 target_ip) {
    u8 packet[42];
    /* Dest MAC */
    net_memcpy(packet + 0, target_mac, 6);
    /* Src MAC */
    net_memcpy(packet + 6, g_net.mac, 6);
    /* EtherType: 0x0806 (ARP) */
    packet[12] = 0x08; packet[13] = 0x06;

    /* ARP Payload */
    packet[14] = 0x00; packet[15] = 0x01; /* HW: Ethernet (1) */
    packet[16] = 0x08; packet[17] = 0x00; /* Proto: IPv4 (0x0800) */
    packet[18] = 0x06;                    /* HW size: 6 */
    packet[19] = 0x04;                    /* Proto size: 4 */
    packet[20] = (u8)(opcode >> 8);
    packet[21] = (u8)(opcode & 0xFF);     /* 1 = Request, 2 = Reply */

    /* Sender MAC & IP */
    net_memcpy(packet + 22, g_net.mac, 6);
    packet[28] = (u8)(g_net.ip >> 24);
    packet[29] = (u8)((g_net.ip >> 16) & 0xFF);
    packet[30] = (u8)((g_net.ip >> 8) & 0xFF);
    packet[31] = (u8)(g_net.ip & 0xFF);

    /* Target MAC & IP */
    net_memcpy(packet + 32, target_mac, 6);
    packet[38] = (u8)(target_ip >> 24);
    packet[39] = (u8)((target_ip >> 16) & 0xFF);
    packet[40] = (u8)((target_ip >> 8) & 0xFF);
    packet[41] = (u8)(target_ip & 0xFF);

    net_send_raw(packet, 42);
}

/* Envia pacote IPv4 generico */
int net_send_ipv4(u32 dest_ip, u8 protocol, const void *payload, u16 len) {
    if (!g_net.present || len > 1460) return 0;

    /* Sem o MAC do gateway nao ha como montar o frame: pede ARP e tenta de novo */
    if (!gateway_mac_resolved) {
        net_send_arp(1, (const u8 *)"\xFF\xFF\xFF\xFF\xFF\xFF", g_net.gateway);
        return 0;
    }

    u8 packet[1514];
    /* Todo o trafego sai pelo gateway (QEMU user-mode networking) */
    const u8 *dst_mac = gateway_mac;

    net_memcpy(packet + 0, dst_mac, 6);
    net_memcpy(packet + 6, g_net.mac, 6);
    packet[12] = 0x08; packet[13] = 0x00; /* EtherType IPv4 */

    /* Cabecalho IPv4 (20 bytes) */
    u16 total_len = 20 + len;
    packet[14] = 0x45;                    /* Version 4, IHL 5 */
    packet[15] = 0x00;                    /* ToS */
    packet[16] = (u8)(total_len >> 8);
    packet[17] = (u8)(total_len & 0xFF);
    packet[18] = 0x12; packet[19] = 0x34; /* ID */
    packet[20] = 0x40; packet[21] = 0x00; /* Don't Fragment */
    packet[22] = 64;                      /* TTL = 64 */
    packet[23] = protocol;                /* Protocol (1=ICMP, 17=UDP, 6=TCP) */
    packet[24] = 0x00; packet[25] = 0x00; /* Checksum temporario */

    /* Source IP */
    packet[26] = (u8)(g_net.ip >> 24);
    packet[27] = (u8)((g_net.ip >> 16) & 0xFF);
    packet[28] = (u8)((g_net.ip >> 8) & 0xFF);
    packet[29] = (u8)(g_net.ip & 0xFF);

    /* Dest IP */
    packet[30] = (u8)(dest_ip >> 24);
    packet[31] = (u8)((dest_ip >> 16) & 0xFF);
    packet[32] = (u8)((dest_ip >> 8) & 0xFF);
    packet[33] = (u8)(dest_ip & 0xFF);

    u16 chk = net_checksum(packet + 14, 20);
    packet[24] = (u8)(chk & 0xFF);
    packet[25] = (u8)(chk >> 8);

    /* Payload */
    net_memcpy(packet + 34, payload, len);

    return net_send_raw(packet, 14 + total_len);
}

/* Envia ICMP Echo Request (Ping) */
int net_ping(u32 target_ip) {
    if (!g_net.present) return 0;

    u8 icmp[32];
    icmp[0] = 8;     /* Type: Echo Request */
    icmp[1] = 0;     /* Code: 0 */
    icmp[2] = 0;     /* Checksum zero temporario */
    icmp[3] = 0;
    icmp[4] = 0xAA;  /* Identifier */
    icmp[5] = 0xBB;
    icmp[6] = (u8)(g_net.pings_sent >> 8);
    icmp[7] = (u8)(g_net.pings_sent & 0xFF);

    /* Dados do payload (24 bytes) */
    for (int i = 0; i < 24; i++) icmp[8 + i] = (u8)('A' + i);

    u16 chk = net_checksum(icmp, 32);
    icmp[2] = (u8)(chk & 0xFF);
    icmp[3] = (u8)(chk >> 8);

    g_net.pings_sent++;
    ping_send_time = 0; /* Atualizado externamente ou via ticks */

    /* Se nao temos o MAC do gateway, dispara ARP primeiro */
    if (!gateway_mac_resolved) {
        net_send_arp(1, (const u8 *)"\xFF\xFF\xFF\xFF\xFF\xFF", target_ip);
    }

    return net_send_ipv4(target_ip, IPPROTO_ICMP, icmp, 32);
}

/* Processador de frames recebidos */
static void net_handle_packet(const u8 *pkt, u16 len) {
    if (len < 14) return;

    u16 ethertype = ((u16)pkt[12] << 8) | pkt[13];

    /* Tratamento de ARP */
    if (ethertype == ETHERTYPE_ARP && len >= 42) {
        u16 op = ((u16)pkt[20] << 8) | pkt[21];
        u32 sender_ip = ((u32)pkt[28] << 24) | ((u32)pkt[29] << 16) | ((u32)pkt[30] << 8) | pkt[31];
        u32 target_ip = ((u32)pkt[38] << 24) | ((u32)pkt[39] << 16) | ((u32)pkt[40] << 8) | pkt[41];

        /* Guarda o MAC do Gateway se responder */
        if (sender_ip == g_net.gateway) {
            net_memcpy(gateway_mac, pkt + 22, 6);
            gateway_mac_resolved = 1;
        }

        /* Se for requisicao pedindo nosso IP, envia ARP Reply */
        if (op == 1 && target_ip == g_net.ip) {
            net_send_arp(2, pkt + 22, sender_ip);
        }
        return;
    }

    /* Tratamento de IPv4 */
    if (ethertype == ETHERTYPE_IPV4 && len >= 34) {
        u8 proto = pkt[23];
        u32 src_ip = ((u32)pkt[26] << 24) | ((u32)pkt[27] << 16) | ((u32)pkt[28] << 8) | pkt[29];
        u32 dst_ip = ((u32)pkt[30] << 24) | ((u32)pkt[31] << 16) | ((u32)pkt[32] << 8) | pkt[33];

        if (dst_ip != g_net.ip && dst_ip != 0xFFFFFFFF) return;

        /* IHL e tamanho real do payload IP (ignora padding/CRC do Ethernet) */
        u16 ip_hdr = (u16)((pkt[14] & 0x0F) * 4);
        u16 ip_total = ((u16)pkt[16] << 8) | pkt[17];
        if (ip_hdr < 20 || ip_hdr > (u16)(len - 14)) return;
        u16 ip_payload = (ip_total >= ip_hdr) ? (u16)(ip_total - ip_hdr) : (u16)(len - 14 - ip_hdr);
        if (ip_payload > (u16)(len - 14 - ip_hdr)) ip_payload = (u16)(len - 14 - ip_hdr);
        const u8 *l4 = pkt + 14 + ip_hdr;

        /* ICMP */
        if (proto == IPPROTO_ICMP && ip_payload >= 8) {
            const u8 *icmp = l4;
            u8 type = icmp[0];
            if (type == 0) { /* Echo Reply */
                g_net.pings_received++;
                g_net.last_ping_ms = 1; /* Latencia local virtual QEMU ~1ms */
            } else if (type == 8) { /* Echo Request para nos -> Responde com Reply */
                u16 icmp_len = ip_payload;
                u8 reply[128];
                if (icmp_len > 120) icmp_len = 120;
                net_memcpy(reply, icmp, icmp_len);
                reply[0] = 0; /* Type 0 = Echo Reply */
                reply[2] = 0; reply[3] = 0;
                u16 chk = net_checksum(reply, icmp_len);
                reply[2] = (u8)(chk & 0xFF);
                reply[3] = (u8)(chk >> 8);
                net_send_ipv4(src_ip, IPPROTO_ICMP, reply, icmp_len);
            }
        }
        /* UDP (DNS e futuros apps) */
        else if (proto == IPPROTO_UDP && ip_payload >= 8) {
            net_handle_udp(src_ip, l4, ip_payload);
        }
        /* TCP (HTTP) */
        else if (proto == IPPROTO_TCP && ip_payload >= 20) {
            net_handle_tcp(src_ip, l4, ip_payload);
        }
    }

    /* Repassa para callback de aplicativo se houver */
    if (app_rx_cb) {
        app_rx_cb(pkt, len);
    }
}

/* ============================================================
 *  Camada de transporte: TCP cliente + UDP
 * ============================================================ */

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

#define TCP_ST_CLOSED      0
#define TCP_ST_SYN_SENT    1
#define TCP_ST_ESTABLISHED 2

/* Checksum no formato de rede (big-endian), usado por TCP e UDP */
static u16 net_sum_be(const u8 *p, u32 len, u32 sum) {
    while (len > 1) { sum += ((u16)p[0] << 8) | p[1]; p += 2; len -= 2; }
    if (len) sum += (u16)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)(~sum);
}
static u16 net_checksum_pseudo_be(u32 src, u32 dst, u8 proto, const u8 *data, u32 len) {
    u32 sum = 0;
    sum += (src >> 16) & 0xFFFF; sum += src & 0xFFFF;
    sum += (dst >> 16) & 0xFFFF; sum += dst & 0xFFFF;
    sum += proto; sum += len;
    return net_sum_be(data, len, sum);
}

/* Helpers de string (sem libc) */
static int n_slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int n_copy(char *d, int cap, const char *s) {
    int i = 0;
    if (cap <= 0) return 0;
    while (s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
    return 1;
}
static int n_cat(char *d, int cap, const char *s) {
    int n = n_slen(d);
    if (n >= cap - 1) return 0;
    return n_copy(d + n, cap - n, s);
}
static int n_ieq(const char *a, int alen, const char *b) {
    int i = 0;
    while (i < alen && b[i]) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
        i++;
    }
    return i == alen && b[i] == 0;
}
static int n_starts_ci(const char *s, const char *p) {
    while (*p) {
        char a = *s++, b = *p++;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return 0;
    }
    return 1;
}
static int n_contains_ci(const char *s, int len, const char *p) {
    int pl = n_slen(p);
    for (int i = 0; i + pl <= len; i++) {
        int ok = 1;
        for (int j = 0; j < pl; j++) {
            char a = s[i + j], b = p[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) { ok = 0; break; }
        }
        if (ok) return 1;
    }
    return 0;
}
static int n_parse_ip(const char *s, u32 *out) {
    u32 ip = 0; int part = 0, digits = 0, dots = 0;
    while (*s) {
        if (*s >= '0' && *s <= '9') { part = part * 10 + (*s - '0'); if (part > 255) return 0; digits++; }
        else if (*s == '.') { if (!digits || dots >= 3) return 0; ip = (ip << 8) | (u32)part; part = 0; digits = 0; dots++; }
        else return 0;
        s++;
    }
    if (!digits || dots != 3) return 0;
    ip = (ip << 8) | (u32)part;
    *out = ip;
    return 1;
}
static int n_utoa(u32 v, char *o) {
    char t[12]; int n = 0, k = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) o[k++] = t[--n];
    o[k] = 0;
    return k;
}

/* ---------------- TCP cliente (uma conexao por vez) ---------------- */
static struct {
    int state;
    u16 local_port, remote_port;
    u32 remote_ip;
    u32 snd_nxt, snd_una, rcv_nxt;
    u32 last_tx_ms, connect_ms;
    int retries;
    u8 *rx_buf;
    u32 rx_cap, rx_len;
    u8 tx_buf[1460];
    u16 tx_len;
    u32 tx_seq;
    int tx_pending;
    int rx_full;
    int got_fin;
    int error;
} tcp;

static void tcp_reset(void) {
    tcp.state = TCP_ST_CLOSED;
    tcp.local_port = tcp.remote_port = 0;
    tcp.remote_ip = 0;
    tcp.snd_nxt = tcp.snd_una = tcp.rcv_nxt = 0;
    tcp.last_tx_ms = tcp.connect_ms = 0;
    tcp.retries = 0;
    tcp.rx_buf = 0; tcp.rx_cap = tcp.rx_len = 0;
    tcp.tx_len = 0; tcp.tx_seq = 0; tcp.tx_pending = 0;
    tcp.rx_full = 0; tcp.got_fin = 0; tcp.error = 0;
}

static int tcp_send_segment(u32 seq, u8 flags, const void *data, u16 len) {
    if (!g_net.present) return 0;
    u8 seg[1514];
    u8 hlen = (flags & TCP_SYN) ? 24 : 20;
    if ((u32)hlen + len > sizeof seg) return 0;

    seg[0] = (u8)(tcp.local_port >> 8);  seg[1] = (u8)(tcp.local_port & 0xFF);
    seg[2] = (u8)(tcp.remote_port >> 8); seg[3] = (u8)(tcp.remote_port & 0xFF);
    seg[4] = (u8)(seq >> 24); seg[5] = (u8)(seq >> 16); seg[6] = (u8)(seq >> 8); seg[7] = (u8)seq;
    u32 ack = (flags & TCP_ACK) ? tcp.rcv_nxt : 0;
    seg[8] = (u8)(ack >> 24); seg[9] = (u8)(ack >> 16); seg[10] = (u8)(ack >> 8); seg[11] = (u8)ack;
    seg[12] = (u8)((hlen / 4) << 4);
    seg[13] = flags;
    seg[14] = 0xFA; seg[15] = 0xF0;       /* janela 64240 */
    seg[16] = 0; seg[17] = 0;             /* checksum */
    seg[18] = 0; seg[19] = 0;             /* urgente */

    if (flags & TCP_SYN) {                /* opcao MSS = 1460 */
        seg[20] = 2; seg[21] = 4; seg[22] = 0x05; seg[23] = 0xB4;
    }
    if (len && data) net_memcpy(seg + hlen, data, len);

    u16 c = net_checksum_pseudo_be(g_net.ip, tcp.remote_ip, IPPROTO_TCP, seg, (u32)hlen + len);
    seg[16] = (u8)(c >> 8); seg[17] = (u8)(c & 0xFF);

    return net_send_ipv4(tcp.remote_ip, IPPROTO_TCP, seg, (u16)(hlen + len));
}

static int tcp_connect(u32 ip, u16 port, u8 *buf, u32 cap) {
    tcp_reset();
    tcp.remote_ip = ip;
    tcp.remote_port = port;
    tcp.local_port = (u16)(40000 + (net_ms % 20000));
    tcp.rx_buf = buf;
    tcp.rx_cap = cap;
    tcp.connect_ms = net_ms;
    tcp.last_tx_ms = net_ms;

    u32 isn = (net_ms * 1103515245u + 12345u) ^ 0xA5A5A5A5u;
    if (!isn) isn = 0x12345678u;
    tcp.snd_nxt = isn;
    tcp.snd_una = isn;
    tcp.rcv_nxt = 0;
    tcp.state = TCP_ST_SYN_SENT;

    tcp_send_segment(isn, TCP_SYN, 0, 0);   /* se o ARP ainda nao resolveu, tcp_tick reenvia */
    tcp.snd_nxt = isn + 1;
    return 1;
}

static int tcp_send(const void *data, u16 len) {
    if (tcp.state != TCP_ST_ESTABLISHED) return 0;
    const u8 *p = (const u8 *)data;
    u16 left = len;
    while (left) {
        u16 n = left > 1440 ? 1440 : left;
        u32 seq = tcp.snd_nxt;
        if (!tcp_send_segment(seq, TCP_PSH | TCP_ACK, p, n)) return 0;
        tcp.snd_nxt += n;
        if (n <= sizeof tcp.tx_buf) {
            net_memcpy(tcp.tx_buf, p, n);
            tcp.tx_len = n;
            tcp.tx_seq = seq;
            tcp.tx_pending = 1;
        }
        tcp.last_tx_ms = net_ms;
        tcp.retries = 0;
        p += n;
        left -= n;
    }
    return 1;
}

static void tcp_close(void) {
    if (tcp.state == TCP_ST_ESTABLISHED) {
        tcp_send_segment(tcp.snd_nxt, TCP_RST | TCP_ACK, 0, 0);
    }
    tcp.state = TCP_ST_CLOSED;
    tcp.tx_pending = 0;
}

/* Encerramento educado (FIN) quando o conteudo ja foi recebido por completo */
static void tcp_finish(void) {
    if (tcp.state == TCP_ST_ESTABLISHED) {
        tcp_send_segment(tcp.snd_nxt, TCP_FIN | TCP_ACK, 0, 0);
        tcp.snd_nxt++;
    }
    tcp.state = TCP_ST_CLOSED;
    tcp.tx_pending = 0;
}

static void net_tcp_tick(void) {
    if (tcp.state == TCP_ST_SYN_SENT) {
        if (net_ms - tcp.last_tx_ms > 1500) {
            if (tcp.retries < 8) {
                tcp.retries++;
                tcp.last_tx_ms = net_ms;
                tcp_send_segment(tcp.snd_nxt - 1, TCP_SYN, 0, 0);
            } else if (net_ms - tcp.connect_ms > 12000) {
                tcp.state = TCP_ST_CLOSED;
                tcp.error = -1;
            }
        }
    } else if (tcp.state == TCP_ST_ESTABLISHED && tcp.tx_pending) {
        if (net_ms - tcp.last_tx_ms > 2000) {
            if (tcp.retries < 5) {
                tcp.retries++;
                tcp.last_tx_ms = net_ms;
                tcp_send_segment(tcp.tx_seq, TCP_PSH | TCP_ACK, tcp.tx_buf, tcp.tx_len);
            } else {
                tcp.state = TCP_ST_CLOSED;
                tcp.error = -1;
            }
        }
    }
}

static void net_handle_tcp(u32 src_ip, const u8 *pkt, u16 len) {
    if (!g_net.present || tcp.state == TCP_ST_CLOSED || len < 20) return;

    u16 sport = ((u16)pkt[0] << 8) | pkt[1];
    u16 dport = ((u16)pkt[2] << 8) | pkt[3];
    if (sport != tcp.remote_port || dport != tcp.local_port || src_ip != tcp.remote_ip) return;

    u32 seq = ((u32)pkt[4] << 24) | ((u32)pkt[5] << 16) | ((u32)pkt[6] << 8) | pkt[7];
    u32 ack = ((u32)pkt[8] << 24) | ((u32)pkt[9] << 16) | ((u32)pkt[10] << 8) | pkt[11];
    u8 flags = pkt[13];
    u8 off = (u8)((pkt[12] >> 4) * 4);
    if (off < 20 || off > len) return;
    u16 plen = (u16)(len - off);
    const u8 *data = pkt + off;

    if (flags & TCP_RST) {
        if (tcp.state == TCP_ST_ESTABLISHED && tcp.rx_len > 0) {
            tcp.got_fin = 1;             /* encerrou depois de entregar dados */
        } else {
            tcp.error = -1;
        }
        tcp.state = TCP_ST_CLOSED;
        return;
    }

    if (tcp.state == TCP_ST_SYN_SENT) {
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) && ack == tcp.snd_nxt) {
            tcp.rcv_nxt = seq + 1;
            tcp.snd_una = ack;
            tcp.state = TCP_ST_ESTABLISHED;
            tcp.retries = 0;
            tcp.tx_pending = 0;
            tcp.last_tx_ms = net_ms;
            tcp_send_segment(tcp.snd_nxt, TCP_ACK, 0, 0);
        }
        return;
    }

    if (tcp.state != TCP_ST_ESTABLISHED) return;

    if (flags & TCP_ACK) {
        if (ack > tcp.snd_una) tcp.snd_una = ack;
        if (tcp.tx_pending && tcp.snd_una >= tcp.tx_seq + tcp.tx_len) tcp.tx_pending = 0;
    }

    if (plen) {
        if (seq == tcp.rcv_nxt) {
            u32 space = tcp.rx_cap - tcp.rx_len;
            u32 n = plen;
            if (n > space) { n = space; tcp.rx_full = 1; }
            if (n && tcp.rx_buf) net_memcpy(tcp.rx_buf + tcp.rx_len, data, n);
            tcp.rx_len += n;
            tcp.rcv_nxt += plen;
            tcp_send_segment(tcp.snd_nxt, TCP_ACK, 0, 0);
        } else {
            tcp_send_segment(tcp.snd_nxt, TCP_ACK, 0, 0);   /* fora de ordem / duplicado */
        }
    }

    if (flags & TCP_FIN) {
        if (seq + plen == tcp.rcv_nxt) {
            tcp.rcv_nxt++;
            tcp.got_fin = 1;
            tcp.state = TCP_ST_CLOSED;
            tcp_send_segment(tcp.snd_nxt, TCP_ACK, 0, 0);
        } else {
            tcp_send_segment(tcp.snd_nxt, TCP_ACK, 0, 0);
        }
    }
}

/* ---------------- UDP ---------------- */
#define DNS_LOCAL_PORT 0xC351

static int net_send_udp(u32 dst_ip, u16 sport, u16 dport, const void *payload, u16 len) {
    if (!g_net.present || len > 1472) return 0;
    u8 udp[8 + 1472];
    u16 total = (u16)(8 + len);
    udp[0] = (u8)(sport >> 8); udp[1] = (u8)(sport & 0xFF);
    udp[2] = (u8)(dport >> 8); udp[3] = (u8)(dport & 0xFF);
    udp[4] = (u8)(total >> 8); udp[5] = (u8)(total & 0xFF);
    udp[6] = 0; udp[7] = 0;
    if (len) net_memcpy(udp + 8, payload, len);
    u16 c = net_checksum_pseudo_be(g_net.ip, dst_ip, IPPROTO_UDP, udp, total);
    if (c == 0) c = 0xFFFF;
    udp[6] = (u8)(c >> 8); udp[7] = (u8)(c & 0xFF);
    return net_send_ipv4(dst_ip, IPPROTO_UDP, udp, total);
}

/* ---------------- DNS ---------------- */
static struct {
    int waiting, done;
    u16 id;
    u32 ip;
    u32 start_ms;
    int retries;
    char host[128];
} dns;

static int dns_skip_name(const u8 *p, u16 len, u16 *pos) {
    while (*pos < len) {
        u8 b = p[*pos];
        if ((b & 0xC0) == 0xC0) { *pos += 2; return *pos <= len; }
        if (b == 0) { (*pos)++; return 1; }
        if (b > 63) return 0;
        *pos += (u16)b + 1;
    }
    return 0;
}

static void dns_handle(const u8 *p, u16 len) {
    if (!dns.waiting || len < 12) return;
    u16 id = ((u16)p[0] << 8) | p[1];
    if (id != dns.id) return;
    u16 flags = ((u16)p[2] << 8) | p[3];
    if (!(flags & 0x8000)) return;
    if ((flags & 0x000F) != 0) { dns.done = -1; dns.waiting = 0; return; }

    u16 qd = ((u16)p[4] << 8) | p[5];
    u16 an = ((u16)p[6] << 8) | p[7];
    u16 pos = 12;
    for (u16 i = 0; i < qd; i++) {
        if (!dns_skip_name(p, len, &pos)) return;
        if (pos + 4 > len) return;
        pos += 4;
    }
    for (u16 i = 0; i < an; i++) {
        if (!dns_skip_name(p, len, &pos)) return;
        if (pos + 10 > len) return;
        u16 type = ((u16)p[pos] << 8) | p[pos + 1];
        u16 cls  = ((u16)p[pos + 2] << 8) | p[pos + 3];
        u16 rdlen = ((u16)p[pos + 8] << 8) | p[pos + 9];
        pos += 10;
        if (pos + rdlen > len) return;
        if (type == 1 && cls == 1 && rdlen == 4) {
            dns.ip = ((u32)p[pos] << 24) | ((u32)p[pos + 1] << 16) | ((u32)p[pos + 2] << 8) | p[pos + 3];
            dns.done = 1; dns.waiting = 0;
            return;
        }
        pos += rdlen;
    }
    dns.done = -1; dns.waiting = 0;
}

static int dns_send_query(void) {
    u8 q[512];
    int n = 0;
    q[n++] = (u8)(dns.id >> 8); q[n++] = (u8)(dns.id & 0xFF);
    q[n++] = 0x01; q[n++] = 0x00;          /* RD */
    q[n++] = 0; q[n++] = 1;                /* QDCOUNT */
    q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0;

    const char *h = dns.host;
    while (*h) {
        const char *dot = h;
        while (*dot && *dot != '.') dot++;
        int l = (int)(dot - h);
        if (l <= 0 || l > 63) return 0;
        if (n + 1 + l >= (int)sizeof q) return 0;
        q[n++] = (u8)l;
        for (int i = 0; i < l; i++) q[n++] = (u8)h[i];
        h = *dot ? dot + 1 : dot;
    }
    if (n + 5 >= (int)sizeof q) return 0;
    q[n++] = 0;
    q[n++] = 0; q[n++] = 1;                /* QTYPE A */
    q[n++] = 0; q[n++] = 1;                /* QCLASS IN */
    return net_send_udp(g_net.dns, DNS_LOCAL_PORT, 53, q, (u16)n);
}

int net_dns_start(const char *host) {
    if (!g_net.present || !host || !*host) return 0;
    n_copy(dns.host, sizeof dns.host, host);
    /* remove ponto final */
    int l = n_slen(dns.host);
    if (l > 1 && dns.host[l - 1] == '.') dns.host[l - 1] = 0;
    dns.id = (u16)((net_ms ^ 0x5A5A) & 0xFFFF);
    if (!dns.id) dns.id = 1;
    dns.waiting = 1;
    dns.done = 0;
    dns.retries = 0;
    dns.start_ms = net_ms;
    dns.ip = 0;
    dns_send_query();
    return 1;
}

int net_dns_poll(void) {
    if (!dns.waiting) return dns.done;
    if (dns.done) { dns.waiting = 0; return dns.done; }
    if (net_ms - dns.start_ms > 1500) {
        if (dns.retries < 4) {
            dns.retries++;
            dns.start_ms = net_ms;
            dns_send_query();
        } else {
            dns.done = -1;
            dns.waiting = 0;
        }
    }
    return dns.done;
}

u32 net_dns_ip(void) { return dns.ip; }

static void net_handle_udp(u32 src_ip, const u8 *pkt, u16 len) {
    (void)src_ip;
    if (len < 8) return;
    u16 sport = ((u16)pkt[0] << 8) | pkt[1];
    u16 dport = ((u16)pkt[2] << 8) | pkt[3];
    u16 ulen  = ((u16)pkt[4] << 8) | pkt[5];
    (void)sport;
    if (ulen < 8 || ulen > len) return;
    if (dport == DNS_LOCAL_PORT) dns_handle(pkt + 8, (u16)(ulen - 8));
}

/* ============================================================
 *  HTTP/1.0 cliente (com DNS, redirects e chunked)
 * ============================================================ */

#define H_IDLE    0
#define H_DNS     1
#define H_CONNECT 2
#define H_RECV    3
#define H_DONE    4
#define H_ERROR   5

static struct http_info hi;
static u32 http_cap;
static u32 http_start_ms, http_last_rx_ms;
static u32 http_last_rx_len;
static int http_header_parsed;
static int http_redirects;

static int http_parse_url(const char *url, char *host, int hostsz, u16 *port,
                          char *path, int pathsz, int *is_https) {
    const char *p = url;
    *is_https = 0;
    while (*p == ' ') p++;
    if (n_starts_ci(p, "http://")) p += 7;
    else if (n_starts_ci(p, "https://")) { *is_https = 1; p += 8; }
    else if (p[0] == '/' && p[1] == '/') p += 2;

    int hn = 0;
    while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#') {
        if (hn + 1 >= hostsz) return 0;
        host[hn++] = *p++;
    }
    host[hn] = 0;
    if (!hn) return 0;

    *port = 80;
    if (*p == ':') {
        p++;
        int v = 0, d = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; d++; }
        if (!d || v <= 0 || v > 65535) return 0;
        *port = (u16)v;
    }

    int pn = 0;
    if (*p == 0 || *p == '?' || *p != '/') {
        if (pn + 1 >= pathsz) return 0;
        path[pn++] = '/';
    }
    while (*p && *p != '#') {
        if (pn + 1 >= pathsz) return 0;
        path[pn++] = *p++;
    }
    path[pn] = 0;
    if (pn == 0) { path[0] = '/'; path[1] = 0; }
    return 1;
}

static int http_resolve_url(const char *base, const char *rel, char *out, int outsz) {
    if (!rel || !*rel) return 0;
    if (n_starts_ci(rel, "http://") || n_starts_ci(rel, "https://")) return n_copy(out, outsz, rel);
    if (rel[0] == '/' && rel[1] == '/') {
        if (!n_copy(out, outsz, "http:")) return 0;
        return n_cat(out, outsz, rel);
    }

    char bhost[128], bpath[256];
    u16 bport; int bhttps;
    if (!http_parse_url(base, bhost, sizeof bhost, &bport, bpath, sizeof bpath, &bhttps)) return 0;

    if (!n_copy(out, outsz, "http://")) return 0;
    if (!n_cat(out, outsz, bhost)) return 0;
    if (bport != 80) {
        char nb[8];
        if (!n_cat(out, outsz, ":")) return 0;
        n_utoa(bport, nb);
        if (!n_cat(out, outsz, nb)) return 0;
    }

    if (rel[0] == '/') {
        if (!n_cat(out, outsz, rel)) return 0;
    } else if (rel[0] == '?') {
        /* mantem o path, troca a query */
        int cut = n_slen(bpath);
        while (cut > 0 && bpath[cut - 1] != '?') cut--;
        if (cut > 0) bpath[cut - 1] = 0;
        if (!n_cat(out, outsz, bpath)) return 0;
        if (!n_cat(out, outsz, rel)) return 0;
    } else if (rel[0] == '#') {
        if (!n_cat(out, outsz, bpath)) return 0;
    } else {
        /* caminho relativo: usa o diretorio do path base */
        char dir[256];
        n_copy(dir, sizeof dir, bpath);
        int cut = n_slen(dir);
        while (cut > 0 && dir[cut - 1] != '/') cut--;
        if (cut == 0) { dir[0] = '/'; dir[1] = 0; }
        else dir[cut] = 0;
        if (!n_cat(out, outsz, dir)) return 0;
        if (!n_cat(out, outsz, rel)) return 0;
    }

    /* remove fragmento */
    for (int i = 0; out[i]; i++) if (out[i] == '#') { out[i] = 0; break; }
    return 1;
}

static u32 http_find_header_end(const u8 *b, u32 n) {
    for (u32 i = 0; i + 3 < n; i++) {
        if (b[i] == '\r' && b[i + 1] == '\n' && b[i + 2] == '\r' && b[i + 3] == '\n') return i + 4;
        if (b[i] == '\n' && b[i + 1] == '\n') return i + 2;
    }
    return 0;
}

static int http_parse_headers(u32 hend) {
    u32 pos = 0;
    char line[256];

    /* linha de status */
    u32 e = pos;
    while (e < hend && hi.buf[e] != '\r' && hi.buf[e] != '\n') e++;
    int ll = (int)(e - pos);
    if (ll > (int)sizeof line - 1) ll = (int)sizeof line - 1;
    for (int i = 0; i < ll; i++) line[i] = (char)hi.buf[pos + i];
    line[ll] = 0;
    if (!n_starts_ci(line, "HTTP/")) return 0;
    int sp = 0;
    while (line[sp] && line[sp] != ' ') sp++;
    while (line[sp] == ' ') sp++;
    int code = 0, cd = 0;
    while (line[sp] >= '0' && line[sp] <= '9' && cd < 3) { code = code * 10 + (line[sp] - '0'); sp++; cd++; }
    if (cd != 3) return 0;
    hi.status = code;

    /* pula o fim da linha de status */
    while (pos < hend && hi.buf[pos] != '\n') pos++;
    if (pos < hend) pos++;

    while (pos < hend) {
        e = pos;
        while (e < hend && hi.buf[e] != '\r' && hi.buf[e] != '\n') e++;
        ll = (int)(e - pos);
        if (ll <= 0) break;
        if (ll > (int)sizeof line - 1) ll = (int)sizeof line - 1;
        for (int i = 0; i < ll; i++) line[i] = (char)hi.buf[pos + i];
        line[ll] = 0;

        int colon = 0;
        while (colon < ll && line[colon] != ':') colon++;
        if (colon < ll) {
            int vs = colon + 1;
            while (vs < ll && (line[vs] == ' ' || line[vs] == '\t')) vs++;
            int ve = ll;
            while (ve > vs && (line[ve - 1] == ' ' || line[ve - 1] == '\t' || line[ve - 1] == '\r')) ve--;
            int vlen = ve - vs;

            if (n_ieq(line, colon, "Content-Length")) {
                u32 v = 0;
                for (int i = vs; i < ve; i++) if (line[i] >= '0' && line[i] <= '9') v = v * 10 + (u32)(line[i] - '0');
                hi.content_length = v;
                hi.has_length = 1;
            } else if (n_ieq(line, colon, "Content-Type")) {
                int n = vlen;
                for (int i = vs; i < ve; i++) if (line[i] == ';') { n = i - vs; break; }
                if (n > (int)sizeof hi.content_type - 1) n = (int)sizeof hi.content_type - 1;
                for (int i = 0; i < n; i++) hi.content_type[i] = line[vs + i];
                hi.content_type[n] = 0;
            } else if (n_ieq(line, colon, "Transfer-Encoding")) {
                if (n_contains_ci(line + vs, vlen, "chunked")) hi.chunked = 1;
            } else if (n_ieq(line, colon, "Location")) {
                int n = vlen;
                if (n > (int)sizeof hi.location - 1) n = (int)sizeof hi.location - 1;
                for (int i = 0; i < n; i++) hi.location[i] = line[vs + i];
                hi.location[n] = 0;
            }
        }

        while (pos < hend && hi.buf[pos] != '\n') pos++;
        if (pos < hend) pos++;
    }
    return 1;
}

static int http_decode_chunked(void) {
    u32 r = hi.body_off;
    u32 w = hi.body_off;
    u32 end = tcp.rx_len;

    while (r < end) {
        u32 sz = 0;
        int digits = 0;
        while (r < end && hi.buf[r] != '\r' && hi.buf[r] != '\n') {
            int c = hi.buf[r++];
            int v = (c >= '0' && c <= '9') ? c - '0'
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (v < 0) return -1;
            sz = sz * 16 + (u32)v;
            if (sz > 0x200000) return -1;
            digits++;
        }
        if (!digits) return -1;
        if (r < end && hi.buf[r] == '\r') r++;
        if (r < end && hi.buf[r] == '\n') r++;

        if (sz == 0) {
            /* trailers: consome ate a linha vazia final */
            while (r < end) {
                if (hi.buf[r] == '\r') { r++; continue; }
                if (hi.buf[r] == '\n') {
                    r++;
                    if (r < end && hi.buf[r] == '\r') { r++; }
                    if (r < end && hi.buf[r] == '\n') { r++; break; }
                    break;
                }
                while (r < end && hi.buf[r] != '\n') r++;
                if (r < end) r++;
            }
            hi.body_len = w - hi.body_off;
            return 1;
        }

        if (r + sz > end) return 0;      /* chunk incompleto */
        for (u32 i = 0; i < sz; i++) hi.buf[w++] = hi.buf[r++];
        if (r < end && hi.buf[r] == '\r') r++;
        if (r < end && hi.buf[r] == '\n') r++;
    }
    hi.body_len = w - hi.body_off;
    return 0;
}

static int http_send_request(void) {
    char req[512];
    if (!n_copy(req, sizeof req, "GET ")) return 0;
    if (!n_cat(req, sizeof req, hi.path)) return 0;
    if (!n_cat(req, sizeof req, " HTTP/1.0\r\nHost: ")) return 0;
    if (!n_cat(req, sizeof req, hi.host)) return 0;
    if (hi.port != 80) {
        char nb[8];
        n_utoa(hi.port, nb);
        if (!n_cat(req, sizeof req, ":")) return 0;
        if (!n_cat(req, sizeof req, nb)) return 0;
    }
    if (!n_cat(req, sizeof req, "\r\nUser-Agent: ElusBrowser/1.0 (Elus OS)\r\n"
                                 "Accept: text/html,text/plain;q=0.9,*/*;q=0.8\r\n"
                                 "Accept-Language: pt-BR,pt;q=0.9,en;q=0.8\r\n"
                                 "Connection: close\r\n\r\n")) return 0;
    return tcp_send(req, (u16)n_slen(req));
}

static int http_start_url(const char *url, int redirects) {
    u8 *buf = hi.buf;
    u32 cap = http_cap;
    tcp_close();
    net_memset(&hi, 0, sizeof hi);
    hi.buf = buf;
    http_cap = cap;
    http_redirects = redirects;
    http_header_parsed = 0;
    http_last_rx_len = 0;
    http_start_ms = http_last_rx_ms = net_ms;

    if (!n_copy(hi.url, sizeof hi.url, url)) { hi.state = H_ERROR; hi.error = HTTP_ERR_URL; return 0; }

    char host[128], path[256];
    u16 port; int https;
    if (!http_parse_url(url, host, sizeof host, &port, path, sizeof path, &https)) {
        hi.state = H_ERROR; hi.error = HTTP_ERR_URL; return 0;
    }
    if (https) { hi.state = H_ERROR; hi.error = HTTP_ERR_HTTPS; return 0; }

    n_copy(hi.host, sizeof hi.host, host);
    n_copy(hi.path, sizeof hi.path, path);
    hi.port = port;

    u32 ip;
    if (n_parse_ip(host, &ip)) {
        hi.ip = ip;
        hi.state = H_CONNECT;
        tcp_connect(ip, port, buf, cap);
    } else {
        hi.state = H_DNS;
        net_dns_start(host);
    }
    return 1;
}

int http_get(const char *url, u8 *buf, u32 cap) {
    if (!g_net.present || !buf || cap < 4096) return 0;
    http_cap = cap;
    hi.buf = buf;
    return http_start_url(url, 0);
}

/* Resolve uma URL relativa contra uma base (usado pelos links do navegador) */
int http_resolve(const char *base, const char *rel, char *out, int outsz) {
    return http_resolve_url(base, rel, out, outsz);
}

void http_abort(void) {
    tcp_close();
    hi.state = H_IDLE;
    hi.error = HTTP_ERR_ABORTED;
}

int http_is_busy(void) {
    return hi.state == H_DNS || hi.state == H_CONNECT || hi.state == H_RECV;
}

const struct http_info *http_get_info(void) {
    return &hi;
}

void http_poll(void) {
    net_tcp_tick();
    net_dns_poll();

    if (hi.state == H_DNS) {
        int r = net_dns_poll();
        if (r == 1) {
            hi.ip = net_dns_ip();
            hi.state = H_CONNECT;
            http_start_ms = net_ms;
            tcp_connect(hi.ip, hi.port, hi.buf, http_cap);
        } else if (r < 0) {
            hi.state = H_ERROR;
            hi.error = HTTP_ERR_DNS;
        }
        return;
    }

    if (hi.state == H_CONNECT) {
        if (tcp.state == TCP_ST_ESTABLISHED) {
            if (!http_send_request()) {
                hi.state = H_ERROR;
                hi.error = HTTP_ERR_PROTOCOL;
                return;
            }
            hi.state = H_RECV;
            http_last_rx_ms = net_ms;
        } else if (tcp.state == TCP_ST_CLOSED && tcp.error) {
            hi.state = H_ERROR;
            hi.error = HTTP_ERR_CONNECT;
        } else if (net_ms - http_start_ms > 10000) {
            tcp_close();
            hi.state = H_ERROR;
            hi.error = HTTP_ERR_TIMEOUT;
        }
        return;
    }

    if (hi.state != H_RECV) return;

    hi.received = tcp.rx_len;
    if (tcp.rx_len != http_last_rx_len) {
        http_last_rx_len = tcp.rx_len;
        http_last_rx_ms = net_ms;
    }

    if (tcp.error) {
        hi.state = H_ERROR;
        hi.error = HTTP_ERR_CONNECT;
        return;
    }

    if (!http_header_parsed) {
        u32 he = http_find_header_end(hi.buf, tcp.rx_len);
        if (he) {
            if (!http_parse_headers(he)) {
                hi.state = H_ERROR;
                hi.error = HTTP_ERR_PROTOCOL;
                return;
            }
            http_header_parsed = 1;
            hi.body_off = he;

            if (hi.status >= 300 && hi.status < 400 && hi.location[0] && http_redirects < 5) {
                char newurl[384];
                if (http_resolve_url(hi.url, hi.location, newurl, sizeof newurl)) {
                    http_start_url(newurl, http_redirects + 1);
                    return;
                }
            }
            if (hi.status == 204 || hi.status == 304) {
                hi.body_len = 0;
                hi.state = H_DONE;
                tcp_finish();
                return;
            }
        } else if (tcp.rx_len > 8192) {
            hi.state = H_ERROR;
            hi.error = HTTP_ERR_PROTOCOL;
            return;
        }
    }

    if (http_header_parsed) {
        if (tcp.rx_full) {
            hi.body_len = tcp.rx_len > hi.body_off ? tcp.rx_len - hi.body_off : 0;
            hi.truncated = 1;
            hi.state = H_DONE;
            tcp_finish();
            return;
        }

        if (hi.chunked) {
            int r = http_decode_chunked();
            if (r == 1) { hi.state = H_DONE; tcp_finish(); return; }
            if (r < 0) { hi.state = H_ERROR; hi.error = HTTP_ERR_PROTOCOL; return; }
        } else if (hi.has_length) {
            u32 avail = tcp.rx_len > hi.body_off ? tcp.rx_len - hi.body_off : 0;
            hi.body_len = avail;
            if (avail >= hi.content_length) {
                hi.body_len = hi.content_length;
                hi.state = H_DONE;
                tcp_finish();
                return;
            }
        } else {
            hi.body_len = tcp.rx_len > hi.body_off ? tcp.rx_len - hi.body_off : 0;
        }

        if (tcp.state == TCP_ST_CLOSED && tcp.got_fin) {
            if (!hi.chunked) hi.body_len = tcp.rx_len > hi.body_off ? tcp.rx_len - hi.body_off : 0;
            hi.state = H_DONE;
            return;
        }
    }

    if (net_ms - http_last_rx_ms > 15000) {
        if (http_header_parsed && tcp.rx_len > hi.body_off) {
            /* entrega o que chegou, mesmo sem FIN/Content-Length */
            if (!hi.chunked) hi.body_len = tcp.rx_len - hi.body_off;
            hi.truncated = 1;
            hi.state = H_DONE;
            tcp_finish();
        } else {
            tcp_close();
            hi.state = H_ERROR;
            hi.error = HTTP_ERR_TIMEOUT;
        }
    }
}

/* Polling do anel de recepcao da placa */
static volatile int net_in_poll;

void net_poll(void) {
    if (!g_net.present || net_in_poll) return;
    net_in_poll = 1;

    /* Enquanto houver pacotes (bit 0 do CR indica se o buffer nao esta vazio) */
    while (!(inb(g_net.io_base + REG_CMD) & 0x01)) {
        u16 offset = rx_offset;
        u8 *hdr = rx_buffer + offset;
        u16 status = *(u16 *)(hdr);
        u16 len = *(u16 *)(hdr + 2);

        if (!(status & 0x01) || len < 4 || len > 1600) break;

        u8 *pkt_data = hdr + 4;
        u16 pkt_len = len - 4; /* Remove 4 bytes do CRC de hardware */

        net_handle_packet(pkt_data, pkt_len);

        g_net.packets_rx++;
        g_net.bytes_rx += pkt_len;

        /* Avanca ponteiro circular alinhado a 4 bytes */
        offset = (offset + len + 4 + 3) & ~3;
        offset %= 8192;
        rx_offset = offset;

        /* Atualiza registrador CAPR da placa (com offset - 0x10) */
        outw(g_net.io_base + REG_CAPR, (offset >= 0x10) ? (offset - 0x10) : (8192 - 0x10 + offset));
    }

    /* Limpa interrupcoes pendentes no registrador ISR */
    outw(g_net.io_base + REG_ISR, 0x0005);
    net_in_poll = 0;
}

void net_irq_handler(void) {
    if (!g_net.present) return;
    /* Apenas reconhece a interrupcao; o processamento acontece no loop
     * principal (evita reentrancia com o parser HTTP). */
    outw(g_net.io_base + REG_ISR, 0xFFFF);
}

/* ============================================================
 *  Inicializacao Geral de Rede
 * ============================================================ */
void net_init(void) {
    struct pci_dev nic_dev;

    /* Procura placa Realtek RTL8139 na barramento PCI */
    if (!pci_find_device(0x10EC, 0x8139, &nic_dev)) {
        g_net.present = 0;
        return;
    }

    /* Habilita Bus Master e espaco de I/O no PCI */
    pci_enable_bus_master(&nic_dev);

    g_net.present = 1;
    g_net.io_base = (u16)(nic_dev.bar0 & ~3);
    g_net.irq = nic_dev.irq;

    /* 1. Liga alimentacao da placa (CONFIG1) */
    outb(g_net.io_base + REG_CONFIG1, 0x00);

    /* 2. Soft Reset no chip */
    outb(g_net.io_base + REG_CMD, 0x10);
    for (int t = 0; t < 10000; t++) {
        if (!(inb(g_net.io_base + REG_CMD) & 0x10)) break;
        io_wait();
    }

    /* 3. Le o endereco fisico MAC (IDR0..IDR5) */
    for (int i = 0; i < 6; i++) {
        g_net.mac[i] = inb(g_net.io_base + REG_MAC0 + i);
    }

    /* 4. Configura Buffer de Recepcao (RBSTART) e ponteiros do anel */
    outl(g_net.io_base + REG_RBSTART, (u32)rx_buffer);
    outw(g_net.io_base + REG_CAPR, 0x0000);
    outw(g_net.io_base + REG_ISR, 0xFFFF);
    rx_offset = 0;
    gateway_mac_resolved = 0;

    /* 5. Habilita Interrupcoes: ROK (Receive OK) e TOK (Transmit OK) */
    outw(g_net.io_base + REG_IMR, 0x0005);

    /* 6. Configura Regras de Recepcao (RCR):
     *    AB (Broadcast) | AM (Multicast) | APM (Physical Match) | AAP (All Packets) | WRAP
     */
    outl(g_net.io_base + REG_RCR, 0x0000008F);

    /* 7. Habilita Receptor e Transmissor (RE e TE) */
    outb(g_net.io_base + REG_CMD, 0x0C);

    g_net.link_up = 1;
    rx_offset = 0;

    /* Envia uma requisicao ARP inicial para descobrir o roteador / gateway */
    net_send_arp(1, (const u8 *)"\xFF\xFF\xFF\xFF\xFF\xFF", g_net.gateway);
}

const struct net_info *net_get_info(void) {
    return &g_net;
}

void net_format_mac(char *out) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 6; i++) {
        out[i * 3]     = hex[(g_net.mac[i] >> 4) & 15];
        out[i * 3 + 1] = hex[g_net.mac[i] & 15];
        if (i < 5) out[i * 3 + 2] = ':';
    }
    out[17] = 0;
}

static int net_utoa_part(int v, char *o) {
    char t[6]; int n = 0, k = 0;
    if (v == 0) t[n++] = '0';
    while (v > 0) { t[n++] = '0' + v % 10; v /= 10; }
    while (n > 0) o[k++] = t[--n];
    return k;
}

void net_format_ip(u32 ip, char *out) {
    int n = 0;
    n += net_utoa_part((int)(ip >> 24) & 255, out + n);
    out[n++] = '.';
    n += net_utoa_part((int)(ip >> 16) & 255, out + n);
    out[n++] = '.';
    n += net_utoa_part((int)(ip >> 8) & 255, out + n);
    out[n++] = '.';
    n += net_utoa_part((int)(ip & 255), out + n);
    out[n] = 0;
}

void net_set_rx_callback(net_rx_callback_t cb) {
    app_rx_cb = cb;
}

void net_tick(void) {
    net_ms += 10;
}

u32 net_time_ms(void) {
    return net_ms;
}

/* ============================================================
 *  Autoteste opcional (compilar com -DNET_SELFTEST)
 *  Faz ARP + ping do gateway e registra o resultado na porta 0xE9.
 * ============================================================ */
#ifdef NET_SELFTEST
static u8 *const st_buf = (u8 *)0x2000000;
static u32 st_last;
static int st_stage;

static void st_dump(const u8 *p, u32 n) {
    for (u32 i = 0; i < n; i++) {
        char c = (char)p[i];
        if (c == '\r' || c == '\n') ndbg(" ");
        else if (c >= 32 && c < 127) { char b[2]; b[0] = c; b[1] = 0; ndbg(b); }
        else ndbg(".");
    }
    ndbg("\n");
}

void net_selftest(void) {
    if (net_ms - st_last < 1000) return;
    st_last = net_ms;

    if (!g_net.present) {
        ndbg("[net] adaptador ausente\n");
        return;
    }

    if (st_stage == 0) {
        ndbg("[net] io="); ndbg_hex(g_net.io_base);
        ndbg(" irq="); ndbg_u32(g_net.irq);
        ndbg(" mac=");
        for (int i = 0; i < 6; i++) {
            char h[3]; h[0] = "0123456789ABCDEF"[g_net.mac[i] >> 4];
            h[1] = "0123456789ABCDEF"[g_net.mac[i] & 15]; h[2] = 0; ndbg(h);
            if (i < 5) ndbg(":");
        }
        ndbg("\n");
        st_stage = 1;
    }

    ndbg("[net] arp="); ndbg_u32((u32)gateway_mac_resolved);
    ndbg(" tx="); ndbg_u32(g_net.packets_tx);
    ndbg(" rx="); ndbg_u32(g_net.packets_rx);
    ndbg(" ping_tx="); ndbg_u32((u32)g_net.pings_sent);
    ndbg(" ping_rx="); ndbg_u32((u32)g_net.pings_received);
    ndbg("\n");

    if (!gateway_mac_resolved) {
        net_send_arp(1, (const u8 *)"\xFF\xFF\xFF\xFF\xFF\xFF", g_net.gateway);
    }
    net_ping(g_net.gateway);

    if (st_stage == 1 && net_ms > 4000) {
        ndbg("[http] GET http://10.0.2.2:8000/\n");
        http_get("http://10.0.2.2:8000/", st_buf, 256 * 1024);
        st_stage = 2;
    } else if (st_stage == 2) {
        const struct http_info *h = http_get_info();
        if (h->state == HTTP_ST_DONE) {
            ndbg("[http] OK status="); ndbg_u32((u32)h->status);
            ndbg(" body="); ndbg_u32(h->body_len);
            ndbg(" type="); ndbg(h->content_type[0] ? h->content_type : "(vazio)");
            ndbg("\n[http] ");
            st_dump(st_buf + h->body_off, h->body_len > 160 ? 160 : h->body_len);
            ndbg("[http] GET http://example.com/\n");
            http_get("http://example.com/", st_buf, 256 * 1024);
            st_stage = 3;
        } else if (h->state == HTTP_ST_ERROR) {
            ndbg("[http] ERRO "); ndbg_u32((u32)(-h->error)); ndbg("\n");
            ndbg("[http] GET http://example.com/\n");
            http_get("http://example.com/", st_buf, 256 * 1024);
            st_stage = 3;
        }
    } else if (st_stage == 3) {
        const struct http_info *h = http_get_info();
        if (h->state == HTTP_ST_DONE) {
            ndbg("[http2] OK status="); ndbg_u32((u32)h->status);
            ndbg(" body="); ndbg_u32(h->body_len);
            ndbg(" type="); ndbg(h->content_type[0] ? h->content_type : "(vazio)");
            ndbg("\n[http2] ");
            st_dump(st_buf + h->body_off, h->body_len > 160 ? 160 : h->body_len);
            st_stage = 4;
        } else if (h->state == HTTP_ST_ERROR) {
            ndbg("[http2] ERRO "); ndbg_u32((u32)(-h->error)); ndbg("\n");
            st_stage = 4;
        }
    }
}
#endif
