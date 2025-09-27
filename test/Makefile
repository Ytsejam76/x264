APP      := test_pskip
SRC      := test_pskip.c
CFLAGS   ?= -O2 -Wall -Wextra -std=c11
LDFLAGS  += $(shell pkg-config --libs libavcodec libavutil)
X264_INC := /home/elias/Documents/Progetti/x264/
X264_LIB := /home/elias/Documents/Progetti/x264/

CFLAGS  += -I$(X264_INC) -I/home/elias/Documents/Progetti/ffmpeg
LDFLAGS += -L$(X264_LIB) -Wl,-rpath,$(X264_LIB) -lx264 -lm


all: $(APP)

$(APP): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(APP) *.o out_*.h264
	rm -rf *_maps

.PHONY: all clean
