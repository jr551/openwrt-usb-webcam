# OpenWrt USB Webcam

Userspace USB webcam capture for OpenWrt routers whose kernel cannot load the normal UVC/V4L2 driver stack.

This project is a workaround for devices where `uvcvideo` cannot be used, for example vendor OpenWrt builds that omit kernel features required by the standard camera stack. The program talks to the webcam directly with `libusb`, captures MJPEG frames from UVC isochronous transfers, and serves the latest frame over HTTP.

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
- `openwrt/usb-webcam.init`: OpenWrt `procd` service script.
- `Makefile`: build helper for compiling on the router.

## Build On Router

The router needs `gcc`, `make`, `libusb`, and the local libusb headers/static library already prepared under `/tmp/usb-cam`:

```sh
cd /tmp/usb-cam
gcc -O2 -I. -Ilibusb-1.0 -Ilibusb_src/libusb -include config.h \
  cam.c build/libusb-1.0.a -o openwrt-usb-webcam
```

The default USB VID/PID is `1bcf:2701`. Override it at compile time for other webcams:

```sh
gcc -O2 -DUSB_WEBCAM_VID=0x1234 -DUSB_WEBCAM_PID=0xabcd ...
```

## Install On Router

```sh
cp openwrt-usb-webcam /usr/local/bin/usb-webcam
cp openwrt/usb-webcam.init /etc/init.d/usb-webcam
chmod +x /usr/local/bin/usb-webcam /etc/init.d/usb-webcam
/etc/init.d/usb-webcam enable
/etc/init.d/usb-webcam restart
```

Then open:

```text
http://<router-ip>:8081/
```

## Notes

This is a pragmatic workaround, not a full UVC stack. It is useful when installing a normal kernel camera driver is not possible.
