ifneq ($(BRDIR),)
CC = $(BRDIR)/output/host/bin/arm-linux-gnueabihf-gcc
SYSROOT = $(BRDIR)/output/staging
LIBS = -lgstreamer-1.0 -lgobject-2.0 -lglib-2.0 -lpthread

all:
	$(CC) v4l2-event-tst.c -o v4l2-event-tst -O2 -Wall \
	    -I$(SYSROOT)/usr/include/gstreamer-1.0 \
	    -I$(SYSROOT)/usr/include/glib-2.0 \
	    -I$(SYSROOT)/usr/lib/glib-2.0/include/ \
	    -L$(SYSROOT)/usr/lib/ \
	    --sysroot=$(SYSROOT) $(LIBS)

else
CFLAGS += $(shell pkg-config --cflags gstreamer-1.0)
LDFLAGS += $(shell pkg-config --libs gstreamer-1.0)

all:
	$(CC) v4l2-event-tst.c -o v4l2-event-tst -O2 -Wall $(CFLAGS) $(LDFLAGS)
endif

.PHONY: clean
clean:
	$(RM) -f *.o v4l2-event-tst
