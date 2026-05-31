#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "libusb.h"

#define EP_IN 0x81
#define IF_VC 0
#define IF_VS 1
#define ALT_SETTING 11
#define PS 3060
#define NP 16
#define NT 4
#define W 320
#define H 240
#define RAWN (W*H*2)
#define MAXF (RAWN*2)
#ifndef USB_WEBCAM_VID
#define USB_WEBCAM_VID 0x1bcf
#endif
#ifndef USB_WEBCAM_PID
#define USB_WEBCAM_PID 0x2701
#endif

static volatile int run = 1;
static libusb_context *ctx;
static libusb_device_handle *h;
static unsigned char *fb;
static int fl;
static unsigned char *jpg;
static int jpg_len;
static int ready;
static int dbg_frames;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;

struct tc { unsigned char b[PS*NP]; int a; struct libusb_transfer *x; };
static struct tc ts[NT];

static void sh(int s) { run = 0; }

static int uvc_set(libusb_device_handle *hd, int i, int cs, int e, unsigned char *d, int n) {
    return libusb_control_transfer(hd, 0x21, 0x01, cs<<8, (e<<8)|i, d, n, 1000);
}

static unsigned char clamp(int x) {
    if (x < 0) return 0;
    if (x > 255) return 255;
    return x;
}

static void publish_raw(unsigned char *p, int n) {
    if (n < RAWN) return;
    FILE *ppm = fopen("/tmp/usb-webcam.ppm", "wb");
    if (!ppm) return;
    fprintf(ppm, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i + 3 < RAWN; i += 4) {
        int y0 = p[i + 0], u = p[i + 1] - 128, y1 = p[i + 2], v = p[i + 3] - 128;
        int r0 = y0 + (1402 * v) / 1000;
        int g0 = y0 - (344 * u + 714 * v) / 1000;
        int b0 = y0 + (1772 * u) / 1000;
        int r1 = y1 + (1402 * v) / 1000;
        int g1 = y1 - (344 * u + 714 * v) / 1000;
        int b1 = y1 + (1772 * u) / 1000;
        fputc(clamp(r0), ppm); fputc(clamp(g0), ppm); fputc(clamp(b0), ppm);
        fputc(clamp(r1), ppm); fputc(clamp(g1), ppm); fputc(clamp(b1), ppm);
    }
    fclose(ppm);
    if (system("/usr/bin/cjpeg -quality 85 /tmp/usb-webcam.ppm > /tmp/usb-webcam.jpg 2>/dev/null") != 0) return;
    FILE *jf = fopen("/tmp/usb-webcam.jpg", "rb");
    if (!jf) return;
    fseek(jf, 0, SEEK_END);
    long sz = ftell(jf);
    fseek(jf, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024*1024) { fclose(jf); return; }
    unsigned char *buf = malloc(sz);
    if (!buf) { fclose(jf); return; }
    if (fread(buf, 1, sz, jf) != (size_t)sz) { fclose(jf); free(buf); return; }
    fclose(jf);
    pthread_mutex_lock(&lk);
    free(jpg);
    jpg = buf;
    jpg_len = sz;
    ready = 1;
    pthread_mutex_unlock(&lk);
}

static LIBUSB_CALL void cbf(struct libusb_transfer *x) {
    struct tc *t = (struct tc *)x->user_data;
    static int last_fid = -1;
    if (!t->a || !run) return;
    if (x->status != LIBUSB_TRANSFER_COMPLETED) { libusb_submit_transfer(x); return; }
    for (int i = 0; i < x->num_iso_packets; i++) {
        int len = x->iso_packet_desc[i].actual_length;
        if (len < 2) continue;
        unsigned char *p = libusb_get_iso_packet_buffer_simple(x, i);
        int hl = p[0];
        if (hl < 2 || hl > len) continue;
        int pl = len - hl;
        if (pl > 0 && fl + pl <= MAXF) {
            memcpy(fb + fl, p + hl, pl);
            fl += pl;
            if (fl >= RAWN) {
                dbg_frames++;
                fprintf(stderr, "raw frame %d complete %d bytes\n", dbg_frames, fl);
                publish_raw(fb, fl);
                fl = 0;
            }
        }
    }
    libusb_submit_transfer(x);
}

static int start(void) {
    unsigned char d[26];
    memset(d, 0, 26);
    d[0] = 1;
    d[2] = 2;    // FORMAT_UNCOMPRESSED YUYV
    d[3] = 2;    // 320x240
    d[4] = 0x80; // 2000000 = 0.5fps
    d[5] = 0x84;
    d[6] = 0x1e;
    int r = uvc_set(h, IF_VS, 0x02, 0, d, 26);
    fprintf(stderr, "PROBE: %d\n", r);
    r = uvc_set(h, IF_VS, 0x03, 0, d, 26);
    fprintf(stderr, "COMMIT: %d\n", r);
    r = libusb_set_interface_alt_setting(h, IF_VS, ALT_SETTING);
    if (r < 0) { fprintf(stderr, "alt: %d\n", r); return -1; }
    fprintf(stderr, "alt setting %d packet %d raw target %d\n", ALT_SETTING, PS, RAWN);
    fb = malloc(MAXF);
    for (int i = 0; i < NT; i++) {
        ts[i].x = libusb_alloc_transfer(NP);
        libusb_fill_iso_transfer(ts[i].x, h, EP_IN, ts[i].b, PS*NP, NP, cbf, &ts[i], 5000);
        libusb_set_iso_packet_lengths(ts[i].x, PS);
        ts[i].a = 1;
        libusb_submit_transfer(ts[i].x);
    }
    return 0;
}

static void handle(int fd) {
    char r[1024]; int n = recv(fd, r, sizeof(r)-1, 0);
    if (n <= 0) { close(fd); return; } r[n] = 0;
    if (strstr(r, "GET /frame")) {
        pthread_mutex_lock(&lk);
        if (ready && jpg && jpg_len > 0) {
            char hdr[128];
            int hl = snprintf(hdr, sizeof(hdr), "HTTP/1.1 200\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\nCache-Control: no-cache\r\n\r\n", jpg_len);
            write(fd, hdr, hl);
            write(fd, jpg, jpg_len);
        } else {
            write(fd, "HTTP/1.1 503\r\n\r\nno frame yet", 29);
        }
        pthread_mutex_unlock(&lk);
    } else {
        const char *html = "HTTP/1.1 200\r\nContent-Type: text/html\r\n\r\n<html><body bgcolor=black><center><img id=cam src=/frame style=max-width:100%><script>setInterval(function(){cam.src='/frame?t='+Date.now()},3000)</script></center></body></html>";
        write(fd, html, strlen(html));
    }
    close(fd);
}

static void *http(void *p) {
    int port = *(int*)p, s = socket(AF_INET, SOCK_STREAM, 0);
    int o = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &o, sizeof(o));
    struct sockaddr_in a = { .sin_family=AF_INET, .sin_port=htons(port), .sin_addr.s_addr=INADDR_ANY };
    if (bind(s, (void*)&a, sizeof(a)) < 0) return NULL;
    listen(s, 4);
    fprintf(stderr, "HTTP port %d\n", port);
    while (run) { struct sockaddr_in ca; socklen_t cl=sizeof(ca); int f=accept(s,(void*)&ca,&cl); if(f>=0) handle(f); }
    close(s); return NULL;
}

int main(int argc, char **argv) {
    int port = 8081;
    for (int o; (o = getopt(argc, argv, "p:h")) != -1;) if (o == 'p') port = atoi(optarg);
    signal(SIGINT, sh); signal(SIGTERM, sh);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (libusb_init(&ctx) < 0) return 1;
    h = libusb_open_device_with_vid_pid(ctx, USB_WEBCAM_VID, USB_WEBCAM_PID);
    if (!h) return 1;
    libusb_detach_kernel_driver(h, IF_VC);
    libusb_detach_kernel_driver(h, IF_VS);
    libusb_claim_interface(h, IF_VC);
    libusb_claim_interface(h, IF_VS);
    pthread_t th; pthread_create(&th, NULL, http, &port);
    if (start() < 0) return 1;
    while (run) { struct timeval tv = {1,0}; libusb_handle_events_timeout(ctx, &tv); }
    return 0;
}
