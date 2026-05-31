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
#define NT 4
#define ALT_SETTING 5
#define PS 800
#define NP 16
#define MAXF (512*1024)
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
static unsigned char *fc;
static int fcl;
static int fr;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static int fc_dbg = 0;

struct tc { unsigned char b[PS*NP]; int a; struct libusb_transfer *x; };
static struct tc ts[NT];

static void sh(int s) { run = 0; }

static int uvc_set(libusb_device_handle *hd, int i, int cs, int e, unsigned char *d, int n) {
    return libusb_control_transfer(hd, 0x21, 0x01, cs<<8, (e<<8)|i, d, n, 1000);
}

static void publish_frame(unsigned char *p, int n) {
    if (n < 1000) return;
    pthread_mutex_lock(&lk);
    if (fc) free(fc);
    fc = malloc(n);
    if (fc) {
        memcpy(fc, p, n);
        fcl = n;
        fr = 1;
        fc_dbg++;
        if (fc_dbg % 30 == 0) fprintf(stderr, "frame %d: %d bytes\n", fc_dbg, fcl);
    }
    pthread_mutex_unlock(&lk);
}

static LIBUSB_CALL void cbf(struct libusb_transfer *x) {
    struct tc *t = (struct tc *)x->user_data;
    if (!t->a || !run) return;
    if (x->status != LIBUSB_TRANSFER_COMPLETED) { libusb_submit_transfer(x); return; }
    
    for (int i = 0; i < x->num_iso_packets; i++) {
        int len = x->iso_packet_desc[i].actual_length;
        if (len < 2) continue;
        unsigned char *p = libusb_get_iso_packet_buffer_simple(x, i);
        int hl = p[0];
        if (hl < 2 || hl > len) continue;
        int pl = len - hl;
        unsigned char *q = p + hl;
        for (int k = 0; k < pl; k++) {
            if (fl == 0) {
                if (q[k] == 0xff) fb[fl++] = q[k];
                continue;
            }
            int prev_ff = (fl > 0 && fb[fl - 1] == 0xff);
            if (prev_ff && q[k] == 0xd8) {
                fb[0] = 0xff;
                fb[1] = 0xd8;
                fl = 2;
                continue;
            }
            if (fl == 1 && fb[0] == 0xff && q[k] != 0xd8) {
                fl = (q[k] == 0xff) ? 1 : 0;
                continue;
            }
            if (fl < MAXF) fb[fl++] = q[k];
            else fl = 0;
            if (prev_ff && q[k] == 0xd9) {
                publish_frame(fb, fl);
                fl = 0;
            }
        }
    }
    libusb_submit_transfer(x);
}

static int start(void) {
    unsigned char d[26];
    memset(d, 0, 26);
    d[0] = 0x01; // bmHint: dwFrameInterval
    d[1] = 0x00;
    d[2] = 1;    // FORMAT_MJPEG
    d[3] = 10;    // 1920x1080
    d[4] = 0x80; // 2000000 = 0.5fps
    d[5] = 0x84;
    d[6] = 0x1e;
    d[7] = 0x00;
    int r = uvc_set(h, IF_VS, 0x02, 0, d, 26);
    fprintf(stderr,"PROBE: %d\n",r);
    r = uvc_set(h, IF_VS, 0x03, 0, d, 26);
    fprintf(stderr,"COMMIT: %d\n",r);
    r = libusb_set_interface_alt_setting(h, IF_VS, ALT_SETTING);
    if (r < 0) { fprintf(stderr,"alt: %d\n",r); return -1; }
    fprintf(stderr,"alt setting %d, packet %d\n", ALT_SETTING, PS);
    
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
    if (n <= 0) { close(fd); return; } r[n]=0;
    if (strstr(r, "GET /index") || (strstr(r, "GET /") && !strstr(r, "GET /frame"))) {
        const char *html = "HTTP/1.1 200\r\nContent-Type: text/html\r\n\r\n"
            "<html><head><title>USB Webcam</title>"
            "</head><body bgcolor=black><center>"
            "<h2 style=\"color:#fff\">USB Webcam</h2>"
            "<img id=\"cam\" src=\"/frame\" style=\"max-width:100%;image-rendering:auto\">"
            "<script>setInterval(function(){document.getElementById('cam').src='/frame?t='+Date.now()},3000)</script>"
            "</center></body></html>";
        write(fd, html, strlen(html));
    } else if (strstr(r, "GET /frame")) {
        pthread_mutex_lock(&lk);
        if (fr && fc && fcl > 0) {
            char hdr[128];
            int hl = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200\r\nContent-Type: image/jpeg\r\n"
                "Content-Length: %d\r\nCache-Control: no-cache\r\n\r\n", fcl);
            write(fd, hdr, hl);
            write(fd, fc, fcl);
        } else {
            write(fd, "HTTP/1.1 503\r\n\r\nno frame yet", 29);
        }
        pthread_mutex_unlock(&lk);
    } else {
        write(fd, "HTTP/1.1 404\r\n\r\n/ /frame", 24);
    }
    close(fd);
}

static void *http(void *p) {
    int port = *(int*)p, s = socket(AF_INET,SOCK_STREAM,0);
    int o = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &o, sizeof(o));
    struct sockaddr_in a = { .sin_family=AF_INET, .sin_port=htons(port), .sin_addr.s_addr=INADDR_ANY };
    if (bind(s,(void*)&a,sizeof(a))<0)return NULL; listen(s,4);
    fprintf(stderr,"HTTP port %d\n",port);
    while(run){struct sockaddr_in ca;socklen_t cl=sizeof(ca);int f=accept(s,(void*)&ca,&cl);if(f>=0)handle(f);}
    close(s); return NULL;
}

int main(int a, char **av) {
    int port = 8081;
    for(int o;(o=getopt(a,av,"p:h"))!=-1;)
        if(o=='p')port=atoi(optarg);
        else{fprintf(stderr,"%s [-p port]\n",av[0]);return 0;}
    signal(SIGINT,sh); signal(SIGTERM,sh);
    setvbuf(stderr,NULL,_IONBF,0);
    fprintf(stderr,"cam port %d\n",port);
    if(libusb_init(&ctx)<0)return 1;
    h=libusb_open_device_with_vid_pid(ctx,USB_WEBCAM_VID,USB_WEBCAM_PID);
    if(!h){fprintf(stderr,"no dev\n");libusb_exit(ctx);return 1;}
    libusb_detach_kernel_driver(h,IF_VC);
    libusb_detach_kernel_driver(h,IF_VS);
    libusb_claim_interface(h,IF_VC);
    libusb_claim_interface(h,IF_VS);
    fprintf(stderr,"dev opened\n");
    pthread_t t; pthread_create(&t,NULL,http,&port);
    if(start()<0)goto out;
    fprintf(stderr,"streaming http://<ip>:%d/\n",port);
    while(run){struct timeval tv={1,0};libusb_handle_events_timeout(ctx,&tv);}
    fprintf(stderr,"shutdown\n");
out:
    libusb_close(h); libusb_exit(ctx);
    return 0;
}
