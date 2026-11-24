# Relay Server 빌드
#   make          : 디버그 빌드(ASan · UBSan) build/relay-server
#   make release  : -O2, sanitizer 없음 build/release/relay-server
#   make dist     : release와 같은 목적 파일을 정적 링크 build/dist/relay-server(배포 VM에 파일 하나로 올린다)
#   make test     : 단위 테스트 전부 → 통합 테스트(서버를 --port 0으로 띄워 시나리오 (a)–(m) 실행)
#   make soak     : 접속 · 끊기 되풀이. 릴리스 서버 SOAK_SECONDS(300)초 RSS 추세,
#                   ASan 서버 SOAK_ASAN_SECONDS(60)초 누수 검사. 오래 걸려 test에 넣지 않는다
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
SOAK_OBJS := $(B)/obj/tests/soak/soak_main.o $(B)/obj/tests/integration/client.o \
	$(B)/obj/tests/integration/udp_client.o $(B)/obj/tests/integration/server_proc.o
SOAK_BIN := $(B)/tests/soak
SOAK_SECONDS ?= 300
SOAK_ASAN_SECONDS ?= 60

# sanitizer가 무엇이든 찾으면 바로 0이 아닌 코드로 끝나게 한다. 누수 검사(LeakSanitizer)도 켠다.
export ASAN_OPTIONS := detect_leaks=1:abort_on_error=0:strict_string_checks=1
export UBSAN_OPTIONS := halt_on_error=1:print_stacktrace=1

.PHONY: all release dist test soak clean

all: $(B)/relay-server

# 패턴 규칙으로 만든 테스트 목적 파일을 make가 중간 산출물로 보고 지우지 않게 한다.
.SECONDARY:

release: $(B)/release/relay-server

dist: $(B)/dist/relay-server

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

# 배포 VM의 glibc 버전과 상관없이 돌도록 정적 링크한다. file로 "statically linked"를 보여 준다.
$(B)/dist/relay-server: $(REL_OBJS)
	@mkdir -p $(dir $@)
	$(CC) -static $^ -o $@
	file $@

$(B)/tests/%: $(B)/obj/tests/unit/%.o $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(DEBUG_FLAGS) $^ -o $@

$(ITEST_BIN): $(ITEST_OBJS) $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(DEBUG_FLAGS) $^ -o $@

# soak는 통합 테스트 클라이언트를 그대로 쓴다.
$(B)/obj/tests/soak/soak_main.o: INC += -Itests/integration

$(SOAK_BIN): $(SOAK_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(DEBUG_FLAGS) $^ -o $@

test: $(UNIT_BINS) $(ITEST_BIN) $(B)/relay-server
	@set -e; for t in $(UNIT_BINS); do echo "== $$t"; ./$$t; done
	@echo "== $(ITEST_BIN)"
	./$(ITEST_BIN) ./$(B)/relay-server

soak: $(SOAK_BIN) $(B)/release/relay-server $(B)/relay-server
	./$(SOAK_BIN) ./$(B)/release/relay-server $(SOAK_SECONDS) release
	./$(SOAK_BIN) ./$(B)/relay-server $(SOAK_ASAN_SECONDS) asan

clean:
	rm -rf $(B)

-include $(OBJS:.o=.d) $(REL_OBJS:.o=.d) $(ITEST_OBJS:.o=.d) $(SOAK_OBJS:.o=.d) \
	$(UNIT_SRCS:%.c=$(B)/obj/%.d)
