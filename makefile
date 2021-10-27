bin:=$(notdir $(shell pwd))
src:=$(shell ls *.c)
obj:=$(src:.c=.o)

libs:=luajit

CFLAGS?=\
	-march=native -O3 -flto \
	-Wall -Wextra -pedantic \
	-D_GNU_SOURCE

LDFLAGS?=\
	-O3 -s -flto \
	-lmosquitto

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
