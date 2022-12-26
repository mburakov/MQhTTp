bin:=$(notdir $(shell pwd))
src:=$(shell ls *.c)
obj:=$(src:.c=.o)

obj+=\
	toolbox/buffer.o \
	toolbox/http_parser.o \
	toolbox/io_muxer.o \
	toolbox/mqtt.o \
	toolbox/mqtt_parser.o \
	toolbox/utils.o

libs:=\
	luajit

CFLAGS+=$(shell pkg-config --cflags $(libs))
LDFLAGS+=$(shell pkg-config --libs $(libs))

all: $(bin)

$(bin): $(obj)
	$(CC) $^ $(LDFLAGS) -o $@

%.o: %.c *.h
	$(CC) -c $< $(CFLAGS) -o $@

clean:
	-rm $(bin) $(obj)

.PHONY: all clean
