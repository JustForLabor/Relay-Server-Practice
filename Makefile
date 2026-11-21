# Relay Server 빌드
#   make          : 디버그 빌드(ASan · UBSan) build/relay-server
#   make release  : -O2, sanitizer 없음 build/release/relay-server
#   make test     : 단위 테스트 전부 → 통합 테스트(서버를 --port 0으로 띄워 시나리오 실행)
#   make clean    : build/ 삭제
# 산출물과 내려받은 uthash.h는 모두 build/ 아래에만 둔다(커밋하지 않는다).

CC := gcc
WARN := -std=c11 -Wall -Wextra -Wpedantic -Werror -D_GNU_SOURCE
DEBUG_FLAGS := -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
RELEASE_FLAGS := -O2

B := build
UTHASH_URL := https://raw.githubusercontent.com/troydhanson/uthash/v2.3.0/src/uthash.h
UTHASH := $(B)/uthash/uthash.h
INC := -Iserver/src -I$(B)/uthash

# 게임 모듈(server/src/games/*.c)도 재귀로 잡아, 새 게임 파일을 Makefile 수정 없이 포함한다.
SRCS := $(shell find server/src -name '*.c' | sort)
OBJS := $(SRCS:%.c=$(B)/obj/%.o)
REL_OBJS := $(SRCS:%.c=$(B)/release/obj/%.o)
# 테스트는 main.o만 빼고 서버 목적 파일 전부와 링크한다.
LIB_OBJS := $(filter-out $(B)/obj/server/src/main.o,$(OBJS))

UNIT_SRCS := $(sort $(wildcard tests/unit/test_*.c))
UNIT_BINS := $(UNIT_SRCS:tests/unit/%.c=$(B)/tests/%)
ITEST_SRCS := $(sort $(wildcard tests/integration/*.c))
ITEST_OBJS := $(ITEST_SRCS:%.c=$(B)/obj/%.o)
ITEST_BIN := $(B)/tests/integration_test

# sanitizer가 무엇이든 찾으면 바로 0이 아닌 코드로 끝나게 한다. 누수 검사(LeakSanitizer)도 켠다.
export ASAN_OPTIONS := detect_leaks=1:abort_on_error=0:strict_string_checks=1
export UBSAN_OPTIONS := halt_on_error=1:print_stacktrace=1

.PHONY: all release test clean

all: $(B)/relay-server

# 패턴 규칙으로 만든 테스트 목적 파일을 make가 중간 산출물로 보고 지우지 않게 한다.
.SECONDARY:

release: $(B)/release/relay-server

$(UTHASH):
	@mkdir -p $(dir $@)
	curl -fsSL -o $@ $(UTHASH_URL)

$(B)/obj/%.o: %.c | $(UTHASH)
	@mkdir -p $(dir $@)
	$(CC) $(WARN) $(DEBUG_FLAGS) $(INC) -MMD -MP -c $< -o $@

$(B)/release/obj/%.o: %.c | $(UTHASH)
	@mkdir -p $(dir $@)
	$(CC) $(WARN) $(RELEASE_FLAGS) $(INC) -MMD -MP -c $< -o $@

$(B)/relay-server: $(OBJS)
	$(CC) $(DEBUG_FLAGS) $^ -o $@

$(B)/release/relay-server: $(REL_OBJS)
	$(CC) $^ -o $@

$(B)/tests/%: $(B)/obj/tests/unit/%.o $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(DEBUG_FLAGS) $^ -o $@

$(ITEST_BIN): $(ITEST_OBJS) $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(DEBUG_FLAGS) $^ -o $@

test: $(UNIT_BINS) $(ITEST_BIN) $(B)/relay-server
	@set -e; for t in $(UNIT_BINS); do echo "== $$t"; ./$$t; done
	@echo "== $(ITEST_BIN)"
	./$(ITEST_BIN) ./$(B)/relay-server

clean:
	rm -rf $(B)

-include $(OBJS:.o=.d) $(REL_OBJS:.o=.d) $(ITEST_OBJS:.o=.d) \
	$(UNIT_SRCS:%.c=$(B)/obj/%.d)
