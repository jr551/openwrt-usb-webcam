# OpenWrt USB Webcam

Userspace USB webcam capture for OpenWrt routers whose kernel cannot load the normal UVC/V4L2 driver stack.

This was built for a GL.iNet GL-BE9300 running a QSDK OpenWrt build where `uvcvideo` cannot load because the kernel lacks `DMA_SHARED_BUFFER`. The program talks to the webcam directly with `libusb`, captures MJPEG frames from UVC isochronous transfers, and serves the latest frame over HTTP.

## Current Mode

- Format: MJPEG
- Resolution: 1920x1080
- Refresh: roughly every 3 seconds
- HTTP endpoint: `/frame`
- Browser page: `/`

The code intentionally uses a conservative USB alternate setting (`ALT_SETTING 5`, packet size `800`) because it produced complete full-frame images more reliably than the highest-bandwidth settings.

## Files

- `src/openwrt-usb-webcam.c`: deployed MJPEG capture and HTTP server.
- `src/openwrt-usb-webcam-yuyv.c`: experimental raw YUYV snapshot path.
- `openwrt/backdoor-cam.init`: OpenWrt `procd` service script.
- `Makefile`: build helper for compiling on the router.

## Build On Router

The router needs `gcc`, `make`, `libusb`, and the local libusb headers/static library already prepared under `/tmp/usb-cam`:

```sh
cd /tmp/usb-cam
gcc -O2 -I. -Ilibusb-1.0 -Ilibusb_src/libusb -include config.h \
  cam.c build/libusb-1.0.a -o backdoor-cam
```

## Install On Router

```sh
cp backdoor-cam /usr/local/bin/backdoor-cam
cp openwrt/backdoor-cam.init /etc/init.d/backdoor-cam
chmod +x /usr/local/bin/backdoor-cam /etc/init.d/backdoor-cam
/etc/init.d/backdoor-cam enable
/etc/init.d/backdoor-cam restart
```

Then open:

```text
http://<router-ip>:8081/
```

## Notes

This is a pragmatic workaround, not a full UVC stack. It is useful when installing a normal kernel camera driver is not possible.
