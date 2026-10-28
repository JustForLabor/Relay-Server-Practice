# Relay-Server-Practice

C로 작성한 경량 틱 기반 게임 릴레이 서버와, 이를 사용하는 Unity Pong 데모 구현을 목표로 하는 프로젝트

> 상태: 개발 중 (마감 2026-12-06)

## 목표

- 방(Room) 단위로 클라이언트를 묶어 패킷을 중계하는 서버 구현
- 고정 틱 루프 기반의 권위적(authoritative) 상태 동기화
- 바이너리 패킷 프로토콜 설계 및 문서화
- 더미 클라이언트를 이용한 부하 테스트와 지연 시간 측정
- Unity Pong 2인 대전 데모로 실제 사용 사례 검증

## 구조

```
Relay-Server-Practice/
├── server/          # C 릴레이 서버
├── client-unity/    # Unity Pong 데모
├── tools/           # 더미 클라이언트, 벤치마크 도구
├── tests/           # 단위/통합 테스트
└── docs/            # 프로토콜 명세, 설계 기록(ADR), 측정 보고서
```

## 빌드 및 실행

개발 환경: Linux 또는 WSL2 (epoll 사용 예정)

```bash
# 클론
git clone https://github.com/JustForLabor/Relay-Server-Practice.git
cd Relay-Server-Practice

# 빌드 (예정)
make

# 서버 실행 (예정)
./build/relay-server --port 7777
```

## 의존성

소켓, epoll 이벤트 루프, 패킷 처리는 직접 구현하고, 기본 자료구조만 외부 라이브러리를 사용한다.

| 라이브러리 | 용도 | 라이선스 |
|---|---|---|
| [uthash](https://github.com/troydhanson/uthash) | 해시 테이블 (방 목록, 클라이언트 목록 등) | BSD |
| [stb_ds.h](https://github.com/nothings/stb) | 동적 배열 (필요 시) | Public Domain / MIT |

## 로드맵

- [ ] 1단계: 에코 서버, 채팅방 서버 (다중 접속, 브로드캐스트)
- [ ] 2단계: 패킷 프로토콜, 방 관리
- [ ] 3단계: 고정 틱 루프와 상태 동기화
- [ ] 4단계: Unity Pong 데모 연동
- [ ] 5단계: 부하 테스트, 측정 보고서
- [ ] v1.0 릴리스

## 문서

- 프로토콜 명세: `docs/protocol.md` (작성 예정)
- 설계 결정 기록: `docs/adr/` (작성 예정)
- 측정 보고서: `docs/report.md` (작성 예정)

## 라이선스

[MIT License](LICENSE)
