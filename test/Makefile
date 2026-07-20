APP      := test_pskip
SRC      := test_pskip.c
CFLAGS   ?= -O2 -Wall -Wextra -std=c11
PKG_CONFIG ?= pkg-config

FFMPEG_CFLAGS := $(shell $(PKG_CONFIG) --cflags libavcodec libavutil)
FFMPEG_LIBS := $(shell $(PKG_CONFIG) --libs libavcodec libavutil)

CFLAGS  += -I.. $(FFMPEG_CFLAGS)
LDFLAGS += -L.. -Wl,-rpath,../ -lx264 -lm $(FFMPEG_LIBS)

all: $(APP)

$(APP): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(APP) *.o out_*.h264 reg_*.h264 reg_*.mp4 reg_*_source.yuv
	rm -rf *_maps

.PHONY: all clean
