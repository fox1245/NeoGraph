"""국회의장 — Python equivalent of speaker.cpp.

Same pattern: discover each member via AgentCard, broadcast the bill
in parallel via A2AClient, parse `투표: 찬성/반대/기권` out of each
reply, tally and announce. Uses the v0.2.1 a2a Python binding.

Usage:
    python speaker.py <bill_file> <member_url> [<member_url> ...]
"""

import re
import sys
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass

import neograph_engine as ng

VOTE_LINE = re.compile(r"투표\s*[:：]\s*(찬성|반대|기권)")


@dataclass
class MemberResult:
    url: str
    name: str = ""
    party: str = "?"
    reply: str = ""
    vote: str = "(불명)"


def parse_vote(reply: str) -> str:
    m = VOTE_LINE.search(reply)
    if m:
        return m.group(1)
    # Fallback: first occurrence wins.
    candidates = [(reply.find(v), v) for v in ("찬성", "반대", "기권")]
    candidates = [(p, v) for p, v in candidates if p != -1]
    if not candidates:
        return "(불명)"
    return min(candidates)[1]


def extract_party(description: str) -> str:
    m = re.search(r"\(([^)]+)\)", description or "")
    return m.group(1) if m else "?"


def call_member(url: str, prompt: str) -> MemberResult:
    r = MemberResult(url=url)
    try:
        client = ng.a2a.A2AClient(url)
        client.set_timeout(60)
        card = client.fetch_agent_card()
        r.name = card.name
        r.party = extract_party(card.description)
        task = client.send_message(prompt)
        if task.history:
            for part in task.history[-1].parts:
                if part.kind == "text":
                    r.reply += part.text
        r.vote = parse_vote(r.reply)
    except Exception as e:  # noqa: BLE001
        r.reply = f"(통신 오류) {e}"
    return r


def main(bill_path: str, member_urls: list[str]) -> int:
    bill = open(bill_path, encoding="utf-8").read()

    print("\n╔══════════════════════════════════════════════════════╗")
    print("║          AI 국회 — 본회의 (Python edition)           ║")
    print("╚══════════════════════════════════════════════════════╝\n")
    print("[국회의장] 의안 상정:")
    print("-" * 58)
    print(bill)
    print("-" * 58, "\n")

    prompt = (
        bill
        + "\n\n위 법안에 대해 다음을 답하세요:\n"
        + "1) 의견 요지 (3-5문장)\n"
        + "2) 마지막 줄에 정확히 다음 형식으로 표시: "
        + "`투표: 찬성` 또는 `투표: 반대` 또는 `투표: 기권`\n"
    )

    with ThreadPoolExecutor(max_workers=len(member_urls)) as ex:
        results = list(ex.map(lambda u: call_member(u, prompt), member_urls))

    print("[국회의장] 토론 + 투표:\n")
    for r in results:
        print(f"─ {r.party} {r.name} ({r.url}) ─")
        print(r.reply)
        print(f"→ 투표 인식: {r.vote}\n")

    tally = {"찬성": 0, "반대": 0, "기권": 0, "(불명)": 0}
    for r in results:
        tally[r.vote] = tally.get(r.vote, 0) + 1
    print("[국회의장] 표결 결과:")
    print(f"  찬성: {tally['찬성']}")
    print(f"  반대: {tally['반대']}")
    print(f"  기권: {tally['기권']}")
    if tally["(불명)"]:
        print(f"  (불명): {tally['(불명)']}")

    if tally["찬성"] > tally["반대"]:
        verdict = "본 법안은 가결되었습니다."
    elif tally["반대"] > tally["찬성"]:
        verdict = "본 법안은 부결되었습니다."
    else:
        verdict = "찬반 동수입니다 — 본 법안은 부결됩니다 (관례)."
    print(f"\n[국회의장] {verdict}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <bill_file> <member_url> ...", file=sys.stderr)
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2:]))
