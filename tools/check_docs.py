#!/usr/bin/env python3
"""docs/ 아래 마크다운의 그림 규칙을 검사한다.

1. 글자로 그린 그림 금지: 상자 그리기 문자(U+2500-U+257F)와 화살표 도형(▲▼◀▶►◄),
   그리고 코드 블록 밖 · 언어 없는 코드 블록 안의 ASCII 상자(`+----+`)와 긴 화살표(`--->`, `<---`)는
   한글과 섞이면 폭이 어긋나 깨져 보인다. 그림은 mermaid로, 아니면 표 · 목록으로 쓴다.
2. 코드 블록에는 언어를 적는다(프로그램 출력은 `text`). 언어 없는 블록은 글자 그림이 숨는 곳이라 금지한다.
3. mermaid는 위에서 아래로: flowchart/graph는 TB 또는 TD만, stateDiagram은
   `direction TB`를 반드시 적는다(가로로 넓어지면 화면에 맞춰 글자가 작아진다).
4. 사람이 읽는 설계 문서(READER_DOCS)의 그림은 쉬운 말로: 라벨에 코드 이름(snake_case,
   4자 이상 영문 대문자 단어)을 쓰지 않고, 상태 그림에서 같은 선 라벨을 3번 이상 반복하지 않는다
   (모든 상태에 공통인 전이는 글 한 줄로).
   코드 이름은 그림 밑 표에 "그림 이름 → 코드 이름"으로 둔다.

사용: python3 tools/check_docs.py [경로 ...]   (기본: docs)
위반이 있으면 파일:줄과 이유를 출력하고 종료 코드 1.
"""
import pathlib
import re
import sys

BOX = re.compile(r"[\u2500-\u257F\u25B2\u25BC\u25C0\u25B6\u25BA\u25C4]")
ASCII_ART = re.compile(r"\+-{3,}|-{3,}>|<-{3,}")
CODE_LANGS = ("c", "csharp", "cs", "bash", "sh", "powershell", "python", "make", "makefile", "json")
FLOW = re.compile(r"^\s*(flowchart|graph)\b\s*(\w+)?")
STATE = re.compile(r"^\s*stateDiagram(-v2)?\b")
DIRECTION = re.compile(r"^\s*direction\s+(\w+)")
READER_DOCS = ("overview.md",)
QUOTED = re.compile(r'"([^"]*)"')
EDGE_LABEL = re.compile(r"\|([^|]*)\|")
STATE_ALIAS = re.compile(r'^\s*state\s+"[^"]*"\s+as\s+(\w+)')
STATE_EDGE = re.compile(r"^\s*(\S+)\s*-->\s*([^\s:]+)\s*(?::\s*(.*))?$")
CODE_NAME = re.compile(r"\b[A-Za-z0-9]+_[A-Za-z0-9_]+\b|\b[A-Z]{4,}\b")


def check_reader_block(path, start, lines):
    """규칙 4: 사람이 읽는 그림의 라벨 검사. lines = [(줄 번호, 내용), ...]"""
    errors, labels, shown = [], [], []
    aliases = {"[*]"}
    is_state = any(STATE.match(t) for _, t in lines)
    for no, text in lines:
        a = STATE_ALIAS.match(text)
        if a:
            aliases.add(a.group(1))
    for no, text in lines:
        for q in QUOTED.findall(text):
            shown.append((no, q))
        for e in EDGE_LABEL.findall(text):
            shown.append((no, e))
            labels.append(e.strip())
        if is_state:
            m = STATE_EDGE.match(text)
            if m:
                for sid in (m.group(1), m.group(2)):
                    if sid not in aliases:
                        shown.append((no, sid))
                if m.group(3):
                    shown.append((no, m.group(3)))
                    labels.append(m.group(3).strip())
    for no, text in shown:
        hit = CODE_NAME.search(text.replace("<br/>", " "))
        if hit:
            errors.append(f"{path}:{no}: 그림 라벨에 코드 이름 금지 (그림 밑 표로): {hit.group(0)}")
    for lab in set(labels) if is_state else ():
        if lab and labels.count(lab) >= 3:
            errors.append(f"{path}:{start}: 같은 선 라벨이 {labels.count(lab)}번 반복된다 (글 한 줄로): {lab}")
    return errors


def check_file(path: pathlib.Path) -> list[str]:
    errors = []
    fence = None  # None: 코드 블록 밖, 문자열: 열린 블록의 언어("" = 언어 없음)
    state_start = None
    state_has_tb = False

    reader = path.name in READER_DOCS
    block = []
    block_start = 0

    for no, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        stripped = line.strip()
        if stripped.startswith("```"):
            if fence is None:
                fence = stripped[3:].strip().lower()
                state_start, state_has_tb = None, False
                block, block_start = [], no
                if not fence:
                    errors.append(f"{path}:{no}: 코드 블록에 언어가 없다 (출력은 ```text, 그림은 ```mermaid)")
            else:
                if state_start is not None and not state_has_tb:
                    errors.append(f"{path}:{state_start}: stateDiagram에 `direction TB`가 없다")
                if reader and fence == "mermaid":
                    errors += check_reader_block(path, block_start, block)
                fence = None
            continue
        if fence == "mermaid":
            block.append((no, line))

        if BOX.search(line):
            errors.append(f"{path}:{no}: 선 문자 그림 금지 (mermaid로 그린다): {stripped[:60]}")
        elif fence not in CODE_LANGS and fence != "mermaid" and ASCII_ART.search(line):
            errors.append(f"{path}:{no}: ASCII 그림 금지 (mermaid로 그린다): {stripped[:60]}")

        if fence != "mermaid":
            continue
        m = FLOW.match(line)
        if m and (m.group(2) or "TB").upper() not in ("TB", "TD"):
            errors.append(f"{path}:{no}: mermaid {m.group(1)}는 TB/TD만 쓴다 (지금: {m.group(2)})")
        if STATE.match(line):
            state_start, state_has_tb = no, False
        d = DIRECTION.match(line)
        if d:
            if d.group(1).upper() in ("TB", "TD"):
                state_has_tb = True
            else:
                errors.append(f"{path}:{no}: mermaid direction은 TB만 쓴다 (지금: {d.group(1)})")
    return errors


def main(argv: list[str]) -> int:
    roots = [pathlib.Path(a) for a in argv] or [pathlib.Path("docs")]
    files = []
    for root in roots:
        files += [root] if root.is_file() else sorted(root.rglob("*.md"))
    errors = [e for f in files for e in check_file(f)]
    for e in errors:
        print(e)
    print(f"check_docs: {len(files)} files, {len(errors)} violations")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
