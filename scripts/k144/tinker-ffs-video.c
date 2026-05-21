// tinker-ffs-video.c — TT #621 W6 dedicated video transport.
//
// Variant of tinker-ffs-control.c that owns /dev/usb-ffs/video and
// bridges its bulk endpoints to TCP 10001 (StackFlow) on a separate
// connection.  Used by Tab5's voice_yolo path so YOLO frames don't
// contend with the always-on voice/control traffic on ffs.control.
//
// Same protocol, just a different USB endpoint pair + TCP socket.

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>

#define LE32(x) (x)
#define LE16(x) (x)

#define FFS_ROOT       "/dev/usb-ffs/video"
#define EP0_PATH       FFS_ROOT "/ep0"
#define EP_OUT_PATH    FFS_ROOT "/ep1"
#define EP_IN_PATH     FFS_ROOT "/ep2"

#define STACKFLOW_HOST "127.0.0.1"
#define STACKFLOW_PORT 10001

#define BULK_MAX_PACKET_HS  512
#define BULK_MAX_PACKET_SS  1024
#define RELAY_BUF_SIZE      (64 * 1024)   /* bigger than control — JPEG payloads */

#define LOG(fmt, ...)  fprintf(stderr, "[tinker-ffs-vid] " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...) fprintf(stderr, "[tinker-ffs-vid] ERR " fmt ": %s\n", ##__VA_ARGS__, strerror(errno))

struct __attribute__((packed)) func_desc {
    struct usb_interface_descriptor intf;
    struct usb_endpoint_descriptor_no_audio bulk_out;
    struct usb_endpoint_descriptor_no_audio bulk_in;
};

struct __attribute__((packed)) ss_func_desc {
    struct usb_interface_descriptor intf;
    struct usb_endpoint_descriptor_no_audio bulk_out;
    struct usb_ss_ep_comp_descriptor bulk_out_comp;
    struct usb_endpoint_descriptor_no_audio bulk_in;
    struct usb_ss_ep_comp_descriptor bulk_in_comp;
};

struct __attribute__((packed)) descriptors {
    struct usb_functionfs_descs_head_v2 header;
    __le32 fs_count;
    __le32 hs_count;
    __le32 ss_count;
    struct func_desc fs;
    struct func_desc hs;
    struct ss_func_desc ss;
};

#define INTERFACE_TEMPLATE                                                     \
    {                                                                          \
        .bLength            = sizeof(struct usb_interface_descriptor),         \
        .bDescriptorType    = USB_DT_INTERFACE,                                \
        .bInterfaceNumber   = 0,                                               \
        .bNumEndpoints      = 2,                                               \
        .bInterfaceClass    = USB_CLASS_VENDOR_SPEC,                           \
        .bInterfaceSubClass = 0x43,  /* distinct from control's 0x42 */        \
        .bInterfaceProtocol = 0x01,                                            \
        .iInterface         = 1,                                               \
    }

static const struct descriptors DESC = {
    .header = {
        .magic  = LE32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2),
        .length = LE32(sizeof(struct descriptors)),
        .flags  = LE32(FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC
                          | FUNCTIONFS_HAS_SS_DESC),
    },
    .fs_count = LE32(3),
    .hs_count = LE32(3),
    .ss_count = LE32(5),
    .fs = {
        .intf = INTERFACE_TEMPLATE,
        .bulk_out = {
            .bLength=USB_DT_ENDPOINT_SIZE, .bDescriptorType=USB_DT_ENDPOINT,
            .bEndpointAddress=1|USB_DIR_OUT, .bmAttributes=USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize=LE16(64),
        },
        .bulk_in = {
            .bLength=USB_DT_ENDPOINT_SIZE, .bDescriptorType=USB_DT_ENDPOINT,
            .bEndpointAddress=2|USB_DIR_IN, .bmAttributes=USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize=LE16(64),
        },
    },
    .hs = {
        .intf = INTERFACE_TEMPLATE,
        .bulk_out = {
            .bLength=USB_DT_ENDPOINT_SIZE, .bDescriptorType=USB_DT_ENDPOINT,
            .bEndpointAddress=1|USB_DIR_OUT, .bmAttributes=USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize=LE16(BULK_MAX_PACKET_HS),
        },
        .bulk_in = {
            .bLength=USB_DT_ENDPOINT_SIZE, .bDescriptorType=USB_DT_ENDPOINT,
            .bEndpointAddress=2|USB_DIR_IN, .bmAttributes=USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize=LE16(BULK_MAX_PACKET_HS),
        },
    },
    .ss = {
        .intf = INTERFACE_TEMPLATE,
        .bulk_out = {
            .bLength=USB_DT_ENDPOINT_SIZE, .bDescriptorType=USB_DT_ENDPOINT,
            .bEndpointAddress=1|USB_DIR_OUT, .bmAttributes=USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize=LE16(BULK_MAX_PACKET_SS),
        },
        .bulk_out_comp = { .bLength=sizeof(struct usb_ss_ep_comp_descriptor),
                           .bDescriptorType=USB_DT_SS_ENDPOINT_COMP, .bMaxBurst=0 },
        .bulk_in = {
            .bLength=USB_DT_ENDPOINT_SIZE, .bDescriptorType=USB_DT_ENDPOINT,
            .bEndpointAddress=2|USB_DIR_IN, .bmAttributes=USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize=LE16(BULK_MAX_PACKET_SS),
        },
        .bulk_in_comp = { .bLength=sizeof(struct usb_ss_ep_comp_descriptor),
                          .bDescriptorType=USB_DT_SS_ENDPOINT_COMP, .bMaxBurst=0 },
    },
};

static const char IFACE_NAME[] = "Tinker Video";

struct __attribute__((packed)) strings {
    struct usb_functionfs_strings_head header;
    __le16 lang;
    char str0[sizeof(IFACE_NAME)];
};

static const struct strings STRINGS = {
    .header = {
        .magic    = LE32(FUNCTIONFS_STRINGS_MAGIC),
        .length   = LE32(sizeof(struct strings)),
        .str_count = LE32(1),
        .lang_count = LE32(1),
    },
    .lang = LE16(0x0409),
    .str0 = "Tinker Video",
};

static int g_ep0 = -1, g_ep_out = -1, g_ep_in = -1;
static volatile int g_running = 1;
static volatile int g_enabled = 0;

static void on_signal(int s) { (void)s; g_running = 0; }

static int write_descriptors(void) {
    int fd = open(EP0_PATH, O_RDWR);
    if (fd < 0) { LOGE("open %s", EP0_PATH); return -1; }
    if (write(fd, &DESC, sizeof(DESC)) != (ssize_t)sizeof(DESC))   { LOGE("write desc"); close(fd); return -1; }
    if (write(fd, &STRINGS, sizeof(STRINGS)) != (ssize_t)sizeof(STRINGS)) { LOGE("write strings"); close(fd); return -1; }
    LOG("descriptors written, waiting for host enable");
    return fd;
}

static int open_endpoints(void) {
    if (g_ep_out < 0) {
        g_ep_out = open(EP_OUT_PATH, O_RDONLY);
        if (g_ep_out < 0) { LOGE("open %s", EP_OUT_PATH); return -1; }
    }
    if (g_ep_in < 0) {
        g_ep_in = open(EP_IN_PATH, O_WRONLY);
        if (g_ep_in < 0) { LOGE("open %s", EP_IN_PATH); return -1; }
    }
    LOG("endpoints open: OUT=%d IN=%d", g_ep_out, g_ep_in);
    return 0;
}

static const char* ev_name(unsigned t) {
    switch (t) {
        case FUNCTIONFS_BIND: return "BIND"; case FUNCTIONFS_UNBIND: return "UNBIND";
        case FUNCTIONFS_ENABLE: return "ENABLE"; case FUNCTIONFS_DISABLE: return "DISABLE";
        case FUNCTIONFS_SETUP: return "SETUP"; case FUNCTIONFS_SUSPEND: return "SUSPEND";
        case FUNCTIONFS_RESUME: return "RESUME"; default: return "?";
    }
}

static int ep0_pump_once(void) {
    struct usb_functionfs_event events[8];
    ssize_t n = read(g_ep0, events, sizeof(events));
    if (n < 0) { if (errno == EINTR) return 0; LOGE("ep0 read"); return -1; }
    int count = n / sizeof(events[0]);
    for (int i = 0; i < count; i++) {
        unsigned t = events[i].type;
        LOG("ep0 event: %s", ev_name(t));
        switch (t) {
            case FUNCTIONFS_ENABLE:  g_enabled = 1; break;
            case FUNCTIONFS_DISABLE:
            case FUNCTIONFS_UNBIND:  g_enabled = 0; break;
            case FUNCTIONFS_SETUP:
                if (events[i].u.setup.bRequestType & USB_DIR_IN) (void)!write(g_ep0, NULL, 0);
                else                                              (void)!read(g_ep0, NULL, 0);
                break;
            default: break;
        }
    }
    return 0;
}

static int connect_stackflow(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { LOGE("socket"); return -1; }
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(STACKFLOW_PORT);
    inet_pton(AF_INET, STACKFLOW_HOST, &addr.sin_addr);
    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOGE("connect %s:%d", STACKFLOW_HOST, STACKFLOW_PORT);
        close(s); return -1;
    }
    LOG("connected to StackFlow %s:%d", STACKFLOW_HOST, STACKFLOW_PORT);
    return s;
}

struct session { int tcp; volatile int alive; };

static void* relay_usb_to_tcp(void* arg) {
    struct session* s = arg;
    uint8_t buf[RELAY_BUF_SIZE];
    while (s->alive && g_running) {
        ssize_t n = read(g_ep_out, buf, sizeof(buf));
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            LOGE("ep_out read returned %zd", n); break;
        }
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = send(s->tcp, buf + off, n - off, MSG_NOSIGNAL);
            if (w <= 0) { LOGE("tcp send"); goto done; }
            off += w;
        }
    }
done:
    s->alive = 0;
    shutdown(s->tcp, SHUT_RDWR);
    return NULL;
}

static void* relay_tcp_to_usb(void* arg) {
    struct session* s = arg;
    uint8_t buf[RELAY_BUF_SIZE];
    while (s->alive && g_running) {
        ssize_t n = recv(s->tcp, buf, sizeof(buf), 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            LOGE("tcp recv returned %zd", n); break;
        }
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(g_ep_in, buf + off, n - off);
            if (w <= 0) { LOGE("ep_in write"); goto done; }
            off += w;
        }
    }
done:
    s->alive = 0;
    return NULL;
}

int main(void) {
    setlinebuf(stderr);
    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);
    signal(SIGPIPE, SIG_IGN);

    LOG("starting (TT #621 W6 video bridge)");

    mkdir(FFS_ROOT, 0770);
    if (mount("video", FFS_ROOT, "functionfs", 0, NULL) < 0 && errno != EBUSY) {
        LOGE("mount %s", FFS_ROOT); return 1;
    }

    g_ep0 = write_descriptors();
    if (g_ep0 < 0) return 1;

    while (g_running && !g_enabled) if (ep0_pump_once() < 0) return 1;
    if (open_endpoints() < 0) return 1;

    while (g_running) {
        while (g_running && !g_enabled) if (ep0_pump_once() < 0) return 1;
        int tcp = connect_stackflow();
        if (tcp < 0) { sleep(1); continue; }

        struct session s = { .tcp = tcp, .alive = 1 };
        pthread_t t_u2t, t_t2u;
        pthread_create(&t_u2t, NULL, relay_usb_to_tcp, &s);
        pthread_create(&t_t2u, NULL, relay_tcp_to_usb, &s);

        while (s.alive && g_running && g_enabled) {
            struct pollfd pfd = { .fd = g_ep0, .events = POLLIN };
            int pr = poll(&pfd, 1, 500);
            if (pr > 0 && (pfd.revents & POLLIN)) ep0_pump_once();
        }
        s.alive = 0;
        shutdown(tcp, SHUT_RDWR);
        pthread_join(t_u2t, NULL);
        pthread_join(t_t2u, NULL);
        close(tcp);
        LOG("session ended; recycling");
    }

    if (g_ep_out >= 0) close(g_ep_out);
    if (g_ep_in >= 0)  close(g_ep_in);
    close(g_ep0);
    return 0;
}
