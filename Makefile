CC ?= gcc
CFLAGS ?= -O2
LIBUSB_ROOT ?= /tmp/usb-cam

SRC := src/openwrt-usb-webcam.c
YUYV_SRC := src/openwrt-usb-webcam-yuyv.c

CPPFLAGS += -I$(LIBUSB_ROOT) -I$(LIBUSB_ROOT)/libusb-1.0 -I$(LIBUSB_ROOT)/libusb_src/libusb -include $(LIBUSB_ROOT)/config.h
LIBUSB_A := $(LIBUSB_ROOT)/build/libusb-1.0.a

.PHONY: all clean yuyv install

all: openwrt-usb-webcam

openwrt-usb-webcam: $(SRC)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(LIBUSB_A) -o $@

yuyv: openwrt-usb-webcam-yuyv

openwrt-usb-webcam-yuyv: $(YUYV_SRC)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(LIBUSB_A) -o $@

install: openwrt-usb-webcam
	install -m 0755 openwrt-usb-webcam /usr/local/bin/usb-webcam
	install -m 0755 openwrt/usb-webcam.init /etc/init.d/usb-webcam
	/etc/init.d/usb-webcam enable
	/etc/init.d/usb-webcam restart

clean:
	rm -f openwrt-usb-webcam openwrt-usb-webcam-yuyv
