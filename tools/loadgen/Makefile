# loadgen 빌드 (Linux · gcc. 저장소 루트 Makefile과 따로 돈다)
#   make          : -O2 build/loadgen/loadgen (측정에 쓰는 빌드)
#   make debug    : ASan · UBSan build/loadgen/debug/loadgen (도구 자체 점검용)
#   make clean
# O=경로 로 산출물 위치를 바꾼다. bench.sh는 호스트와 공유하는 build/를 건드리지 않게 /tmp를 쓴다.

CC := gcc
WARN := -std=c11 -Wall -Wextra -Wpedantic -Werror -D_GNU_SOURCE
O ?= ../../build/loadgen

SRCS := main.c bot.c botio.c timers.c hist.c report.c
HDRS := lg.h proto.h hist.h

.PHONY: all debug clean

all: $(O)/loadgen

debug: $(O)/debug/loadgen

$(O)/loadgen: $(SRCS) $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(WARN) -O2 $(SRCS) -o $@

$(O)/debug/loadgen: $(SRCS) $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(WARN) -g -fsanitize=address,undefined -fno-sanitize-recover=all \
		-fno-omit-frame-pointer $(SRCS) -o $@

clean:
	rm -rf $(O)
