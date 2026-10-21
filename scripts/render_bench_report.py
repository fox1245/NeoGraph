#!/usr/bin/env python3
"""Render the cross-framework benchmark tables and the chart from a checked-in record.

    scripts/render_bench_report.py                  render from the record named in benchmarks/records/CURRENT
    scripts/render_bench_report.py --check          fail if any rendered file differs from what the record gives
    scripts/render_bench_report.py --record FILE    use another record

The record (benchmarks/records/<date>-compare-<machine>.json, written by benchmarks/compare/driver.py) is the
only input. Every number in a rendered table or chart is computed here from the record's cells; no figure is typed
into the documents. The tables are written between `<!-- bench-record:begin NAME -->` and
`<!-- bench-record:end NAME -->` markers of benchmarks/README.md and its three translations, with the headings of
the tables in each language, and the chart is written to docs/images/bench-engine-overhead.svg.

Statistics. A cell is one fresh process. A round runs every cell once, in a shuffled order. The figure of a cell
in a round is the median of its in-process samples; the table shows the median of that figure across rounds and
the [min, max] across rounds. Ratios divide two such medians. p99 is not published: the samples of a round are
loop means, not individual calls.

Standard library only; Python 3.9+.
"""

from __future__ import annotations

import argparse
import json
import re
import statistics
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RECORDS = ROOT / "benchmarks" / "records"
README = "benchmarks/README"
CHART = ROOT / "docs" / "images" / "bench-engine-overhead.svg"
LOCALES = ("en", "ko", "ja", "zh-CN")

# The packages a cell reports (its port's PACKAGES) are the ones shown in the version labels, in that order.
SHOWN_PACKAGES = {"langgraph": 2, "llamaindex": 2, "autogen": 2}  # how many of the reported packages to show

# ── strings ──────────────────────────────────────────────────────────────────────────────────────────────────────
# Each entry: en, ko, ja, zh-CN. Table headings and the sentences generated from numbers only.

T: dict[str, tuple[str, str, str, str]] = {
    "framework": ("Framework", "프레임워크", "フレームワーク", "框架"),
    "source": ("Source", "출처", "ソース", "来源"),
    "api": ("API measured", "측정한 API", "測定した API", "测量的 API"),
    "seq": ("`seq` µs per run", "`seq` 실행당 µs", "`seq` 1 実行あたり µs", "`seq` 每次运行 µs"),
    "par": ("`par` µs per run", "`par` 실행당 µs", "`par` 1 実行あたり µs", "`par` 每次运行 µs"),
    "seq_ratio": ("`seq` vs. NeoGraph", "`seq` 대 NeoGraph", "`seq` 対 NeoGraph", "`seq` 相对 NeoGraph"),
    "par_ratio": ("`par` vs. NeoGraph", "`par` 대 NeoGraph", "`par` 対 NeoGraph", "`par` 相对 NeoGraph"),
    "release": ("release", "릴리스", "リリース", "发布版"),
    "head": ("HEAD", "HEAD", "HEAD", "HEAD"),
    "na": ("n/a", "해당 없음", "該当なし", "不适用"),
    "ng_w1": ("NeoGraph `worker_count=1`", "NeoGraph `worker_count=1`", "NeoGraph `worker_count=1`",
              "NeoGraph `worker_count=1`"),
    "ng_auto": ("NeoGraph `set_worker_count_auto()`", "NeoGraph `set_worker_count_auto()`",
                "NeoGraph `set_worker_count_auto()`", "NeoGraph `set_worker_count_auto()`"),
    "ng_default": ("NeoGraph `run()` (default `worker_count=1`)", "NeoGraph `run()` (기본 `worker_count=1`)",
                   "NeoGraph `run()` (既定 `worker_count=1`)", "NeoGraph `run()`（默认 `worker_count=1`）"),
    "pool_na_pydantic_graph": (
        "no thread-pool path: steps are coroutines and a plain function is rejected",
        "스레드 풀 경로 없음: 스텝은 코루틴이며 일반 함수는 거부됨",
        "スレッドプール経路なし: ステップはコルーチンで、通常の関数は拒否される",
        "没有线程池路径：步骤是协程，普通函数会被拒绝"),
    "pool_na_llamaindex": (
        "not timed: plain `def` steps are accepted and run on the event loop's default executor, a thread-pool path "
        "that this cohort does not measure",
        "측정하지 않음: 일반 `def` 스텝을 받아 이벤트 루프의 기본 실행기에서 실행하며, 이 코호트는 그 스레드 풀 경로를 "
        "측정하지 않음",
        "未計測: 通常の `def` ステップを受け付けてイベントループの既定のエグゼキューターで実行するスレッドプール経路があるが、"
        "このコホートでは計測していない",
        "未测量：接受普通 `def` 步骤并在事件循环的默认执行器上运行，这条线程池路径不在本次测量范围内"),
    "pool_na_autogen": (
        "no thread-pool path: one single-threaded agent runtime",
        "스레드 풀 경로 없음: 단일 스레드 에이전트 런타임 하나",
        "スレッドプール経路なし: 単一スレッドのエージェントランタイム 1 つ",
        "没有线程池路径：单线程的智能体运行时"),
    "range_line": (
        "NeoGraph `par` is {w1} µs with `worker_count=1` and {auto} µs with `set_worker_count_auto()` "
        "({workers} worker threads).",
        "NeoGraph `par`는 `worker_count=1`에서 {w1} µs, `set_worker_count_auto()`에서 {auto} µs입니다 "
        "(워커 스레드 {workers}개).",
        "NeoGraph の `par` は `worker_count=1` で {w1} µs、`set_worker_count_auto()` で {auto} µs です"
        "（ワーカースレッド {workers} 本）。",
        "NeoGraph 的 `par` 在 `worker_count=1` 时为 {w1} µs，在 `set_worker_count_auto()` 时为 {auto} µs"
        "（{workers} 个工作线程）。"),
    "cell_line": (
        "Figures are the median across {rounds} interleaved rounds with [min, max] across rounds; each round "
        "runs every cell once in a shuffled order, one fresh process per cell. Ratios divide medians.",
        "수치는 {rounds}회 교차 라운드의 중앙값이며 괄호는 라운드 간 [최소, 최대]입니다. 각 라운드는 모든 셀을 "
        "무작위 순서로 한 번씩, 셀마다 새 프로세스로 실행합니다. 비율은 중앙값끼리 나눈 값입니다.",
        "数値は {rounds} 回のインターリーブラウンドの中央値で、括弧はラウンド間の [最小, 最大] です。各ラウンドは"
        "全セルをシャッフルした順で 1 回ずつ、セルごとに新しいプロセスで実行します。比率は中央値同士の割り算です。",
        "数值是 {rounds} 轮交错运行的中位数，括号内是各轮之间的 [最小, 最大]；每轮以打乱的顺序把每个单元各运行一次，"
        "每个单元使用一个全新的进程。比值为中位数相除。"),
    "ceiling_variant": ("Build", "빌드", "ビルド", "构建"),
    "ceiling_hardened": ("default Release build (hardened) — the headline", "기본 Release 빌드(하드닝) — 기준 수치",
                         "既定の Release ビルド（ハードニング）— 見出しの値", "默认 Release 构建（加固）— 基准数值"),
    "ceiling_unhardened": ("unhardened engine ceiling", "하드닝 없는 엔진 상한", "ハードニングなしのエンジン上限",
                           "未加固的引擎上限"),
    "ceiling_ratio": ("hardened ÷ unhardened", "하드닝 ÷ 비하드닝", "ハードニング ÷ 非ハードニング", "加固 ÷ 未加固"),
    "ng_row_w1_seq": ("`seq`, `worker_count=1`", "`seq`, `worker_count=1`", "`seq`, `worker_count=1`",
                      "`seq`, `worker_count=1`"),
    "ng_row_w1_par": ("`par`, `worker_count=1`", "`par`, `worker_count=1`", "`par`, `worker_count=1`",
                      "`par`, `worker_count=1`"),
    "ng_row_auto_par": ("`par`, `set_worker_count_auto()`", "`par`, `set_worker_count_auto()`",
                        "`par`, `set_worker_count_auto()`", "`par`, `set_worker_count_auto()`"),
    "events_row": ("`seq` with every lifecycle event built and delivered to a callback",
                   "`seq`, 모든 수명주기 이벤트를 만들어 콜백으로 전달",
                   "`seq`、すべてのライフサイクルイベントを生成してコールバックに配信",
                   "`seq`，生成每个生命周期事件并交给回调"),
    "flags_title": ("Flags and compiler macros of the default build, as printed by the binary and the build tree:",
                    "기본 빌드의 플래그와 컴파일러 매크로(바이너리와 빌드 트리에서 출력):",
                    "既定ビルドのフラグとコンパイラマクロ（バイナリとビルドツリーの出力）:",
                    "默认构建的标志与编译器宏（由二进制文件和构建树输出）："),
    "mem_framework": ("Framework (HEAD)", "프레임워크 (HEAD)", "フレームワーク (HEAD)", "框架 (HEAD)"),
    "mem_idle": ("Idle: imported, nothing run (MiB)", "유휴: 임포트만, 실행 없음 (MiB)",
                 "アイドル: インポートのみ、実行なし (MiB)", "空闲：仅导入，未运行 (MiB)"),
    "mem_peak_seq": ("Peak during `seq` loops (MiB)", "`seq` 루프 중 최대 (MiB)", "`seq` ループ中のピーク (MiB)",
                     "`seq` 循环期间峰值 (MiB)"),
    "mem_peak_par": ("Peak during `par` loops (MiB)", "`par` 루프 중 최대 (MiB)", "`par` ループ中のピーク (MiB)",
                     "`par` 循环期间峰值 (MiB)"),
    "mem_inc": ("Increase over idle, `seq` / `par` (MiB)", "유휴 대비 증가, `seq` / `par` (MiB)",
                "アイドルからの増加、`seq` / `par` (MiB)", "相对空闲的增量，`seq` / `par` (MiB)"),
    "mem_baseline_python": ("Python interpreter alone, nothing imported", "Python 인터프리터만, 임포트 없음",
                            "Python インタプリタのみ、インポートなし", "仅 Python 解释器，未导入任何内容"),
    "mem_baseline_ng": ("NeoGraph binary: engine built, nothing run", "NeoGraph 바이너리: 엔진 생성, 실행 없음",
                        "NeoGraph バイナリ: エンジン構築済み、実行なし", "NeoGraph 二进制：已构建引擎，未运行"),
    "mem_marginal_title": ("Marginal memory per in-flight run", "실행 중인 run당 한계 메모리",
                           "実行中の run あたりの限界メモリ", "每个在途运行的边际内存"),
    "mem_marginal_cols": ("`seq` KiB per run (RSS / PSS)", "`seq` run당 KiB (RSS / PSS)",
                          "`seq` run あたり KiB (RSS / PSS)", "`seq` 每次运行 KiB (RSS / PSS)"),
    "mem_marginal_cols_par": ("`par` KiB per run (RSS / PSS)", "`par` run당 KiB (RSS / PSS)",
                              "`par` run あたり KiB (RSS / PSS)", "`par` 每次运行 KiB (RSS / PSS)"),
    "cold_framework": ("Framework (HEAD)", "프레임워크 (HEAD)", "フレームワーク (HEAD)", "框架 (HEAD)"),
    "cold_total": ("Process start to first `seq` result, ms", "프로세스 시작부터 첫 `seq` 결과까지, ms",
                   "プロセス開始から最初の `seq` 結果まで、ms", "从进程启动到第一个 `seq` 结果，ms"),
    "cold_parts": ("Import / build / first run (median ms)", "임포트 / 빌드 / 첫 실행 (중앙값 ms)",
                   "インポート / ビルド / 初回実行（中央値 ms）", "导入 / 构建 / 首次运行（中位数 ms）"),
    "size_item": ("Item", "항목", "項目", "项目"),
    "size_unit": ("Unit of deployment", "배포 단위", "デプロイ単位", "部署单位"),
    "size_value": ("Size", "크기", "サイズ", "大小"),
    "size_note": ("What is counted", "집계 대상", "計測対象", "统计内容"),
    "unit_binary": ("one native executable", "네이티브 실행 파일 하나", "ネイティブ実行ファイル 1 つ",
                    "一个原生可执行文件"),
    "unit_shared_set": ("an executable and its shared libraries", "실행 파일과 그 공유 라이브러리",
                        "実行ファイルとその共有ライブラリ", "可执行文件及其共享库"),
    "unit_library": ("one library file", "라이브러리 파일 하나", "ライブラリファイル 1 つ", "一个库文件"),
    "unit_wasm": ("one WebAssembly module", "WebAssembly 모듈 하나", "WebAssembly モジュール 1 つ",
                  "一个 WebAssembly 模块"),
    "unit_venv": ("Python environment: the framework and its whole dependency closure",
                  "Python 환경: 프레임워크와 전체 의존성 묶음", "Python 環境: フレームワークと依存関係の全体",
                  "Python 环境：框架及其全部依赖"),
    "probe_item": ("Probe", "프로브", "プローブ", "探针"),
    "probe_result": ("Result", "결과", "結果", "结果"),
    "failed_cells": ("Cells that failed in this session (excluded from every figure above)",
                     "이 세션에서 실패한 셀(위 모든 수치에서 제외)",
                     "このセッションで失敗したセル（上記のすべての数値から除外）",
                     "本次会话中失败的单元（已从上述所有数值中排除）"),
    "host_line": ("Host: {cpu}, {count} cores, kernel {kernel}, Python {python}, NeoGraph built with {compiler}; "
                  "CPU governor: {governor}.",
                  "호스트: {cpu}, 코어 {count}개, 커널 {kernel}, Python {python}, NeoGraph 빌드 컴파일러 {compiler}; "
                  "CPU 거버너: {governor}.",
                  "ホスト: {cpu}、{count} コア、カーネル {kernel}、Python {python}、NeoGraph のビルドに使ったコンパイラ {compiler}; "
                  "CPU ガバナー: {governor}。",
                  "主机：{cpu}，{count} 核，内核 {kernel}，Python {python}，构建 NeoGraph 的编译器 {compiler}；"
                  "CPU 调频策略：{governor}。"),
    "session_line": ("Session: {start} to {end} UTC; 1-minute load average {before} when the first phase started (after the "
                     "wait for an idle host) and {after} at the end, which includes the session's own processes; "
                     "{rounds} interleaved rounds; record `{record}`.",
                     "세션: {start} ~ {end} UTC; 1분 부하 평균은 첫 단계 시작 시 {before}(유휴 상태를 기다린 뒤), 종료 시 {after}"
                     "(세션 자신의 프로세스 포함); 교차 라운드 {rounds}회; 기록 `{record}`.",
                     "セッション: {start} ～ {end} UTC; 1 分間のロードアベレージは、最初のフェーズの開始時（アイドルになるのを待った後）が "
                     "{before}、終了時が {after}（セッション自身のプロセスを含む）; インターリーブラウンド {rounds} 回; 記録 `{record}`。",
                     "会话：{start} 至 {end} UTC；1 分钟平均负载在第一阶段开始时（等待主机空闲之后）为 {before}，结束时为 {after}"
                     "（含会话自身的进程）；{rounds} 轮交错运行；记录 `{record}`。"),
    # The exception list in this text is a statement about the record that is checked in (benchmarks/records/CURRENT):
    # which harness files differ from the sha256 values that record holds. A new record needs its own text.
    "engine_line": ("NeoGraph engine: commit `{commit}`; harness tree: commit `{harness}`. The harness commit is the "
                    "local commit the session ran from and may not exist in this repository's history; the record is "
                    "never edited, and it holds the sha256 of every harness script, which identifies the files. The "
                    "files in this tree are byte-identical to the hashed ones except `scripts/render_bench_report.py` "
                    "(the version that rendered these tables is hashed below) and `port_llamaindex.py` and "
                    "`port_autogen.py` in `benchmarks/compare/` (one comment each, corrected after the session).",
                    "NeoGraph 엔진: 커밋 `{commit}`; 하니스 트리: 커밋 `{harness}`. 하니스 커밋은 세션을 실행한 로컬 "
                    "커밋이므로 이 저장소의 기록에 없을 수 있습니다. 기록은 수정하지 않으며, 하니스 스크립트마다 sha256이 "
                    "들어 있어 파일을 식별합니다. 이 트리의 파일은 해시된 파일과 바이트 단위로 같습니다. 예외는 "
                    "`scripts/render_bench_report.py`(이 표를 렌더링한 버전의 해시는 아래에 있음)와 "
                    "`benchmarks/compare/`의 `port_llamaindex.py`, `port_autogen.py`(각각 세션 뒤에 주석을 하나 고침)입니다.",
                    "NeoGraph エンジン: コミット `{commit}`; ハーネスツリー: コミット `{harness}`。ハーネスのコミットは"
                    "セッションの実行元のローカルコミットで、このリポジトリの履歴に存在しない場合があります。記録は編集せず、"
                    "記録にはハーネスの各スクリプトの sha256 が入っていて、ファイルを特定できます。このツリーのファイルは、"
                    "ハッシュされたファイルとバイト単位で一致します。例外は `scripts/render_bench_report.py`"
                    "（これらの表を生成したバージョンのハッシュは下に示します）と、`benchmarks/compare/` の "
                    "`port_llamaindex.py`・`port_autogen.py`（それぞれセッション後にコメントを 1 か所修正）です。",
                    "NeoGraph 引擎：提交 `{commit}`；测试工具树：提交 `{harness}`。该测试工具提交是运行本次会话的本地提交，"
                    "可能不在本仓库的历史中；记录不会被修改，其中保存了每个测试工具脚本的 sha256，据此可确定文件。"
                    "本树中的文件与已哈希的文件逐字节相同，例外是 `scripts/render_bench_report.py`"
                    "（渲染这些表格的版本的哈希见下方）以及 `benchmarks/compare/` 中的 `port_llamaindex.py` 和 "
                    "`port_autogen.py`（会话之后各修正了一处注释）。"),
    "render_line": ("Tables rendered by `scripts/render_bench_report.py` (sha256 `{sha}`…).",
                    "표는 `scripts/render_bench_report.py`(sha256 `{sha}`…)가 렌더링했습니다.",
                    "表は `scripts/render_bench_report.py`（sha256 `{sha}`…）が生成しました。",
                    "表格由 `scripts/render_bench_report.py`（sha256 `{sha}`…）渲染。"),
    "c_framework": ("Framework", "프레임워크", "フレームワーク", "框架"),
    "c_release": ("Latest release", "최신 릴리스", "最新リリース", "最新发布版"),
    "c_head": ("HEAD (commit, date, version)", "HEAD (커밋, 날짜, 버전)", "HEAD（コミット、日付、バージョン）",
               "HEAD（提交、日期、版本）"),
    "c_fallback": ("(release used: the HEAD build did not install or import)",
                   "(릴리스 사용: HEAD 빌드가 설치 또는 임포트되지 않음)",
                   "（リリースを使用: HEAD ビルドがインストールまたはインポートできなかった）",
                   "（使用发布版：HEAD 构建无法安装或导入）"),
}

T.update({
    "k_n": ("N", "N", "N", "N"),
    "k_engine": ("Engine and mode", "엔진과 모드", "エンジンとモード", "引擎与模式"),
    "k_wall": ("Wall time of the closed burst", "닫힌 버스트의 벽시계 시간", "クローズドバーストの壁時計時間",
               "封闭突发的墙钟时间"),
    "k_rss": ("Peak RSS, one process (MiB)", "최대 RSS, 프로세스 하나 (MiB)", "ピーク RSS、単一プロセス (MiB)",
              "峰值 RSS，单进程 (MiB)"),
    "k_ok": ("OK / Err", "성공 / 오류", "成功 / エラー", "成功 / 错误"),
    "k_oom": ("OOM killed", "OOM으로 종료", "OOM で強制終了", "因 OOM 被终止"),
    "k_withdrawn": ("withdrawn: the process tree is not counted", "철회: 프로세스 트리를 합산하지 않음",
                    "撤回: プロセスツリーを合算していない", "已撤回：未统计整个进程树"),
    "k_mode": ("{engine} {mode}", "{engine} {mode}", "{engine} {mode}", "{engine} {mode}"),
})

T.update({
    "governor_na": ("not exposed by the host", "호스트가 노출하지 않음", "ホストが公開していない", "主机未提供"),
    "cfg_hardened": ("default hardened Release", "기본 하드닝 Release", "既定のハードニング Release", "默认加固 Release"),
    "cfg_minsize": ("MinSizeRel with LTO (`-Os`)", "MinSizeRel + LTO(`-Os`)", "MinSizeRel と LTO（`-Os`）",
                    "MinSizeRel 加 LTO（`-Os`）"),
    "cfg_unhardened": ("unhardened Release", "하드닝 없는 Release", "ハードニングなしの Release", "未加固 Release"),
    "note_minapp": ("minimal app: registers a node, compiles `seq`, runs it once; stripped",
                    "최소 앱: 노드를 등록하고 `seq`를 컴파일해 한 번 실행; strip 후",
                    "最小のアプリ: ノードを登録し、`seq` をコンパイルして 1 回実行; strip 後",
                    "最小应用：注册一个节点，编译 `seq` 并运行一次；strip 之后"),
    "note_benchbin": ("the benchmark binary; stripped", "벤치마크 바이너리; strip 후", "ベンチマークのバイナリ; strip 後",
                      "基准二进制文件；strip 之后"),
    "note_archive": ("static archive as built", "빌드된 그대로의 정적 아카이브", "ビルドしたままの静的アーカイブ",
                     "按原样构建的静态归档"),
    "note_stripped": ("stripped", "strip 후", "strip 後", "strip 之后"),
    "note_exe_only": ("executable only; stripped", "실행 파일만; strip 후", "実行ファイルのみ; strip 後",
                      "仅可执行文件；strip 之后"),
    "note_shared_deploy": ("executable plus {n} NeoGraph and SDK shared libraries; stripped",
                           "실행 파일과 NeoGraph·SDK 공유 라이브러리 {n}개; strip 후",
                           "実行ファイルと NeoGraph・SDK の共有ライブラリ {n} 個; strip 後",
                           "可执行文件加 {n} 个 NeoGraph 与 SDK 共享库；strip 之后"),
    "item_shared_exe": ("NeoGraph `bench_footprint` (shared `libneograph_core.so`)",
                        "NeoGraph `bench_footprint` (공유 `libneograph_core.so`)",
                        "NeoGraph `bench_footprint`（共有 `libneograph_core.so`）",
                        "NeoGraph `bench_footprint`（共享 `libneograph_core.so`）"),
    "item_shared_deploy": ("NeoGraph shared deployment of the minimal app", "NeoGraph 최소 앱의 공유 라이브러리 배포",
                           "NeoGraph の最小アプリの共有ライブラリ配布", "NeoGraph 最小应用的共享库部署"),
    "wasm_graph_smoke": ("NeoGraph graph smoke `smoke.wasm`", "NeoGraph 그래프 스모크 `smoke.wasm`",
                         "NeoGraph グラフスモーク `smoke.wasm`", "NeoGraph 图冒烟程序 `smoke.wasm`"),
    "wasm_js_module": ("NeoGraph JavaScript module `neograph_module.wasm`", "NeoGraph JavaScript 모듈 `neograph_module.wasm`",
                       "NeoGraph JavaScript モジュール `neograph_module.wasm`", "NeoGraph JavaScript 模块 `neograph_module.wasm`"),
    "note_wasm": ("size gate of the WebAssembly workflow in CI, single-thread job, commit {commit}",
                  "CI의 WebAssembly 워크플로 크기 게이트, 단일 스레드 작업, 커밋 {commit}",
                  "CI の WebAssembly ワークフローのサイズゲート、シングルスレッドのジョブ、コミット {commit}",
                  "CI 中 WebAssembly 工作流的体积关卡，单线程任务，提交 {commit}"),
    "note_venv": ("{n} packages; site-packages minus an empty venv", "패키지 {n}개; site-packages에서 빈 venv를 뺀 값",
                  "パッケージ {n} 個; site-packages から空の venv を引いた値", "{n} 个软件包；site-packages 减去空 venv"),
    "item_ng_lib": ("NeoGraph `{name}` ({cfg})", "NeoGraph `{name}` ({cfg})", "NeoGraph `{name}`（{cfg}）",
                    "NeoGraph `{name}`（{cfg}）"),
    "item_head_install": ("{fw} (HEAD install)", "{fw} (HEAD 설치)", "{fw}（HEAD のインストール）", "{fw}（HEAD 安装）"),
    "mem_ng_row": ("**NeoGraph**: binary with the engine built, nothing run",
                   "**NeoGraph**: 엔진을 만든 바이너리, 실행 없음", "**NeoGraph**: エンジンを構築したバイナリ、実行なし",
                   "**NeoGraph**：已构建引擎的二进制文件，未运行"),
    "reuse_detail": ("sequential runs {seq}, concurrent runs {conc}, llama-index-workflows {version}",
                     "순차 실행 {seq}, 동시 실행 {conc}, llama-index-workflows {version}",
                     "逐次の実行 {seq}、同時の実行 {conc}、llama-index-workflows {version}",
                     "依次运行 {seq}，并发运行 {conc}，llama-index-workflows {version}"),
    "na_sep": ("n/a — ", "해당 없음 — ", "該当なし — ", "不适用 — "),
    "cell": ("cell", "셀", "セル", "单元"),
    "count": ("n", "n", "n", "n"),
    "error": ("error", "오류", "エラー", "错误"),
})

VARIANT_LABELS = {
    "ainvoke_async": ("ainvoke, async def nodes", "ainvoke, async def 노드", "ainvoke、async def ノード", "ainvoke，async def 节点"),
    "invoke_sync": ("invoke, def nodes", "invoke, 일반 def 노드", "invoke、通常の def ノード", "invoke，普通 def 节点"),
    "run_sync": ("Pipeline.run, sync components", "Pipeline.run, 동기 컴포넌트", "Pipeline.run、同期コンポーネント", "Pipeline.run，同步组件"),
    "run_async_coro": ("Pipeline.run_async, async components", "Pipeline.run_async, 코루틴 컴포넌트",
                       "Pipeline.run_async、コルーチンコンポーネント", "Pipeline.run_async，协程组件"),
    "run_async_pool": ("Pipeline.run_async, sync components in threads", "Pipeline.run_async, 스레드에서 실행되는 동기 컴포넌트",
                       "Pipeline.run_async、スレッドで動く同期コンポーネント", "Pipeline.run_async，在线程中运行的同步组件"),
    "builder_steps": ("GraphBuilder steps, real fan-out", "GraphBuilder 스텝, 실제 팬아웃",
                      "GraphBuilder のステップ、本物のファンアウト", "GraphBuilder 步骤，真正的扇出"),
    "basenode_chain": ("BaseNode classes (par is a serial chain)", "BaseNode 클래스(par는 직렬 체인)",
                       "BaseNode クラス（par は直列チェーン）", "BaseNode 类（par 为串行链）"),
    "reused_instance": ("one workflow instance reused", "워크플로 인스턴스 하나를 재사용",
                        "ワークフローインスタンスを 1 つ再利用", "复用同一个工作流实例"),
    "fresh_instance": ("new workflow instance per run", "실행마다 새 워크플로 인스턴스",
                       "実行ごとに新しいワークフローインスタンス", "每次运行新建工作流实例"),
    "reset_run": ("GraphFlow reset and run", "GraphFlow reset 후 run", "GraphFlow を reset して run", "GraphFlow 先 reset 再 run"),
    "fresh_flow": ("new GraphFlow per run", "실행마다 새 GraphFlow", "実行ごとに新しい GraphFlow", "每次运行新建 GraphFlow"),
}


def variant_label(agg, framework: str, variant: str, locale: str) -> str:
    entry = VARIANT_LABELS.get(variant)
    return entry[LOCALES.index(locale)] if entry else agg.plan["frameworks"][framework]["variants"][variant]["label"]


T.update({
    "dr_cfg": ("Configuration", "구성", "構成", "配置"),
    "dr_fanout": ("Fan-out", "팬아웃", "ファンアウト", "扇出"),
    "dr_delay": ("Model delay per call (ms)", "모델 호출당 지연 (ms)", "モデル呼び出しあたりの遅延 (ms)", "每次模型调用的延迟 (ms)"),
    "dr_width": ("Pool width, both sides", "풀 너비, 양쪽 모두", "プールの幅、両側とも", "线程池宽度，两侧相同"),
    "dr_ng": ("NeoGraph, ms per run", "NeoGraph, 실행당 ms", "NeoGraph、1 実行あたり ms", "NeoGraph，每次运行 ms"),
    "dr_lg": ("LangGraph, ms per run", "LangGraph, 실행당 ms", "LangGraph、1 実行あたり ms", "LangGraph，每次运行 ms"),
    "dr_ratio": ("LangGraph ÷ NeoGraph", "LangGraph ÷ NeoGraph", "LangGraph ÷ NeoGraph", "LangGraph ÷ NeoGraph"),
    "dr_A": ("A: no model delay (the engine floor)", "A: 모델 지연 없음(엔진 하한)", "A: モデルの遅延なし（エンジンの下限）",
             "A：无模型延迟（引擎下限）"),
    "dr_B": ("B: five researchers", "B: 연구자 다섯 명", "B: リサーチャー 5 つ", "B：五个研究节点"),
    "dr_C": ("C: fifty researchers, pool as wide as the fan-out", "C: 연구자 50명, 풀 너비가 팬아웃과 같음",
             "C: リサーチャー 50、プールの幅はファンアウトと同じ", "C：五十个研究节点，线程池与扇出等宽"),
    "dr_D": ("D: fifty researchers, narrow pool", "D: 연구자 50명, 좁은 풀", "D: リサーチャー 50、狭いプール",
             "D：五十个研究节点，窄线程池"),
    "dr_line": ("Mock model (a sleep per call), canned search evidence, in-memory checkpoints, no key and no network; "
                "median of {rounds} interleaved rounds with [min, max] across rounds, each round one fresh process per "
                "configuration with {warmup} warmup and {iters} measured runs per side; neograph_engine {version}, "
                "built with {compiler}. Both sides use the same pool width: `NG_WORKER_COUNT` and `LG_MAX_CONCURRENCY`.",
                "모의 모델(호출마다 sleep), 고정된 검색 근거, 메모리 체크포인트, 키와 네트워크 없음; 교차 라운드 {rounds}회의 중앙값과 "
                "라운드 간 [최소, 최대]; 각 라운드는 구성마다 새 프로세스 하나로, 쪽마다 워밍업 {warmup}회와 측정 {iters}회; "
                "neograph_engine {version}, 컴파일러 {compiler}. 양쪽은 같은 풀 너비를 씁니다: `NG_WORKER_COUNT`와 `LG_MAX_CONCURRENCY`.",
                "モックモデル（呼び出しごとのスリープ）、固定の検索エビデンス、インメモリのチェックポイント、キーもネットワークもなし; "
                "{rounds} 回のインターリーブしたラウンドの中央値と、ラウンド間の [最小, 最大]; 各ラウンドは構成ごとに新しいプロセス 1 つで、"
                "片側あたりウォームアップ {warmup} 回と計測 {iters} 回; neograph_engine {version}、コンパイラ {compiler}。"
                "両側は同じプールの幅を使います: `NG_WORKER_COUNT` と `LG_MAX_CONCURRENCY`。",
                "模拟模型（每次调用休眠）、固定的检索证据、内存检查点，无密钥、无网络；{rounds} 轮交错运行的中位数及各轮之间的 [最小, 最大]；"
                "每轮对每个配置使用一个全新进程，每侧预热 {warmup} 次、测量 {iters} 次；neograph_engine {version}，编译器 {compiler}。"
                "两侧使用相同的线程池宽度：`NG_WORKER_COUNT` 与 `LG_MAX_CONCURRENCY`。"),
})

# Reasons printed in the "no probe" cases and probe sentences.
P = {
    "llamaindex_reuse": (
        "LlamaIndex: one `Workflow` instance runs repeatedly and concurrently — {detail}.",
        "LlamaIndex: `Workflow` 인스턴스 하나를 반복 실행하고 동시에도 실행할 수 있음 — {detail}.",
        "LlamaIndex: 1 つの `Workflow` インスタンスを繰り返し、また同時に実行できる — {detail}。",
        "LlamaIndex：同一个 `Workflow` 实例可以重复运行，也可以并发运行 — {detail}。"),
    "llamaindex_reuse_no": (
        "LlamaIndex: one `Workflow` instance could not be reused — {detail}.",
        "LlamaIndex: `Workflow` 인스턴스를 재사용할 수 없음 — {detail}.",
        "LlamaIndex: `Workflow` インスタンスを再利用できなかった — {detail}。",
        "LlamaIndex：`Workflow` 实例无法复用 — {detail}。"),
    "langgraph_threads": (
        "LangGraph: `invoke` runs the five workers on {inv} pool thread(s), none of them the calling thread; "
        "`ainvoke` with `async def` nodes runs them on the calling thread. Timed: `invoke` {inv_par} µs against "
        "`ainvoke` {ainv_par} µs for `par` ({par_ratio}×), {inv_seq} µs against {ainv_seq} µs for `seq` "
        "({seq_ratio}×).",
        "LangGraph: `invoke`는 다섯 워커를 풀 스레드 {inv}개에서 실행하며 호출 스레드는 쓰지 않음; "
        "`async def` 노드의 `ainvoke`는 호출 스레드에서 실행. 측정: `par`는 `invoke` {inv_par} µs 대 "
        "`ainvoke` {ainv_par} µs ({par_ratio}배), `seq`는 {inv_seq} µs 대 {ainv_seq} µs ({seq_ratio}배).",
        "LangGraph: `invoke` は 5 つのワーカーをプールスレッド {inv} 本で実行し、呼び出しスレッドは使わない; "
        "`async def` ノードの `ainvoke` は呼び出しスレッドで実行。計測: `par` は `invoke` {inv_par} µs 対 "
        "`ainvoke` {ainv_par} µs（{par_ratio} 倍）、`seq` は {inv_seq} µs 対 {ainv_seq} µs（{seq_ratio} 倍）。",
        "LangGraph：`invoke` 在 {inv} 个线程池线程上运行五个工作节点，不使用调用线程；使用 `async def` 节点的 "
        "`ainvoke` 在调用线程上运行。实测：`par` 的 `invoke` 为 {inv_par} µs，`ainvoke` 为 {ainv_par} µs"
        "（{par_ratio} 倍）；`seq` 为 {inv_seq} µs 对 {ainv_seq} µs（{seq_ratio} 倍）。"),
    "pydantic_async": (
        "pydantic-graph {version}: `Graph.run` is a coroutine and `run_sync` only drives it on the event loop; a "
        "plain step function is rejected ({sync_error}); five steps that each wait 50 ms finish in {elapsed} ms "
        "on {threads} thread(s), so the fan-out is real and single-threaded.",
        "pydantic-graph {version}: `Graph.run`은 코루틴이고 `run_sync`는 이벤트 루프에서 이를 구동할 뿐; 일반 스텝 "
        "함수는 거부됨({sync_error}); 각각 50 ms 대기하는 스텝 다섯 개가 스레드 {threads}개에서 {elapsed} ms에 "
        "끝나므로 팬아웃은 실제이며 단일 스레드.",
        "pydantic-graph {version}: `Graph.run` はコルーチンで、`run_sync` はイベントループ上で駆動するだけ; "
        "通常のステップ関数は拒否される（{sync_error}）; 各 50 ms 待つステップ 5 つがスレッド {threads} 本で "
        "{elapsed} ms で終わるため、ファンアウトは本物で単一スレッド。",
        "pydantic-graph {version}：`Graph.run` 是协程，`run_sync` 只是在事件循环上驱动它；普通步骤函数会被拒绝"
        "（{sync_error}）；五个各等待 50 ms 的步骤在 {threads} 个线程上于 {elapsed} ms 内完成，因此扇出是真实的，"
        "且为单线程。"),
    "haystack_threads": (
        "Haystack: with sync components, `run_async` offloads the workers to {pool} thread(s) other than the "
        "calling one; with `run_async` components they run on the calling thread, as `run` does.",
        "Haystack: 동기 컴포넌트에서 `run_async`는 워커를 호출 스레드가 아닌 스레드 {pool}개로 넘김; "
        "`run_async` 컴포넌트에서는 `run`처럼 호출 스레드에서 실행.",
        "Haystack: 同期コンポーネントでは `run_async` がワーカーを呼び出しスレッド以外の {pool} 本に任せる; "
        "`run_async` コンポーネントでは `run` と同様に呼び出しスレッドで実行。",
        "Haystack：使用同步组件时，`run_async` 把工作节点交给调用线程之外的 {pool} 个线程；使用 `run_async` 组件时，"
        "与 `run` 一样在调用线程上运行。"),
    "reuse_timing": (
        "LlamaIndex timed: a reused instance {reused_seq} µs and a new instance per run {fresh_seq} µs for `seq`; "
        "{reused_par} µs and {fresh_par} µs for `par`.",
        "LlamaIndex 측정: `seq`는 재사용 인스턴스 {reused_seq} µs, 실행마다 새 인스턴스 {fresh_seq} µs; "
        "`par`는 {reused_par} µs와 {fresh_par} µs.",
        "LlamaIndex 計測: `seq` は再利用インスタンス {reused_seq} µs、実行ごとに新規 {fresh_seq} µs; "
        "`par` は {reused_par} µs と {fresh_par} µs。",
        "LlamaIndex 实测：`seq` 复用实例 {reused_seq} µs，每次运行新建实例 {fresh_seq} µs；"
        "`par` 为 {reused_par} µs 与 {fresh_par} µs。"),
    "hardening": (
        "Hardening delta of `bench_neograph`: the default hardened build against the unhardened build, `seq` "
        "{seq_ratio}×, `par` with `worker_count=1` {par_ratio}×, `par` with the worker pool {auto_ratio}×.",
        "`bench_neograph`의 하드닝 차이: 기본 하드닝 빌드 대 비하드닝 빌드, `seq` {seq_ratio}배, "
        "`worker_count=1`의 `par` {par_ratio}배, 워커 풀의 `par` {auto_ratio}배.",
        "`bench_neograph` のハードニング差: 既定のハードニングビルド対非ハードニングビルド、`seq` {seq_ratio} 倍、"
        "`worker_count=1` の `par` {par_ratio} 倍、ワーカープールの `par` {auto_ratio} 倍。",
        "`bench_neograph` 的加固差异：默认加固构建相对未加固构建，`seq` {seq_ratio} 倍，`worker_count=1` 的 `par` "
        "{par_ratio} 倍，工作线程池的 `par` {auto_ratio} 倍。"),
    "pydantic_legacy_logfire": (
        "The same legacy pydantic-graph loop with the optional `logfire` package installed in the environment: "
        "{lf} µs per run, {ratio}× the clean environment ({clean} µs). Its instrumentation spans are no-ops "
        "without the package and real spans with it, so a shared environment that contains it changes the figure.",
        "같은 레거시 pydantic-graph 루프에 선택 패키지 `logfire`를 환경에 설치하면 run당 {lf} µs로, 깨끗한 환경 "
        "({clean} µs)의 {ratio}배. 이 패키지가 없으면 계측 스팬은 아무 일도 하지 않고, 있으면 실제 스팬이 되므로 "
        "이를 포함한 공유 환경은 수치를 바꿈.",
        "同じレガシー pydantic-graph のループで、オプションの `logfire` パッケージを環境にインストールすると "
        "1 run あたり {lf} µs で、クリーンな環境（{clean} µs）の {ratio} 倍。計測スパンはパッケージがなければ何もせず、"
        "あれば実際のスパンになるため、これを含む共有環境では数値が変わる。",
        "同样的旧版 pydantic-graph 循环，在环境中安装可选包 `logfire` 后为每次运行 {lf} µs，是干净环境"
        "（{clean} µs）的 {ratio} 倍。没有该包时埋点 span 是空操作，有该包时是真实的 span，因此包含它的共享环境会"
        "改变数值。"),
    "pydantic_legacy": (
        "pydantic-graph 1.x, legacy `Graph(nodes=[...])` API, measured back to back on this host: a loop of awaited "
        "runs {loop} µs per run, the same runs as gathered tasks {burst} µs, the loop inside one task {task} µs, "
        "with the cyclic collector off {gc} µs. The shape of the harness does not explain the older 227 µs against "
        "70 to 100 µs: the two figures came from different sessions and environments.",
        "pydantic-graph 1.x, 레거시 `Graph(nodes=[...])` API, 이 호스트에서 연달아 측정: await 루프 {loop} µs/run, "
        "같은 run을 gather한 태스크 {burst} µs, 하나의 태스크 안의 루프 {task} µs, 순환 GC 끈 경우 {gc} µs. "
        "하니스의 형태로는 기존의 227 µs 대 70~100 µs를 설명할 수 없으며, 두 수치는 서로 다른 세션과 환경에서 나왔음.",
        "pydantic-graph 1.x、レガシー `Graph(nodes=[...])` API、このホストで連続計測: await ループ {loop} µs/run、"
        "同じ run を gather したタスク {burst} µs、1 つのタスク内のループ {task} µs、循環 GC オフ {gc} µs。"
        "ハーネスの形では従来の 227 µs 対 70〜100 µs を説明できず、2 つの数値は別々のセッションと環境のものである。",
        "pydantic-graph 1.x，旧版 `Graph(nodes=[...])` API，在本主机上连续测量：await 循环 {loop} µs/次，同样的运行"
        "以 gather 任务方式 {burst} µs，单个任务内的循环 {task} µs，关闭循环垃圾回收 {gc} µs。测试工具的形态"
        "无法解释此前的 227 µs 对 70 至 100 µs：这两个数值来自不同的会话与环境。"),
}

SECTION_TITLES = {  # used only inside the footprint table notes
}


def tr(key: str, locale: str, **kwargs) -> str:
    entry = T[key] if key in T else P[key]
    text = entry[LOCALES.index(locale)]
    return text.format(**kwargs) if kwargs else text


# ── record access ────────────────────────────────────────────────────────────────────────────────────────────────

class Stat:
    def __init__(self, values: list[float]):
        self.values = values
        self.median = statistics.median(values)
        self.low = min(values)
        self.high = max(values)
        self.n = len(values)


class Agg:
    def __init__(self, record: dict):
        self.record = record
        self.plan = record["plan"]
        self.cells = record["cells"]
        self.by_id: dict[str, list[dict]] = {}
        self.failed: list[dict] = []
        for cell in self.cells:
            if cell["status"] == "ok":
                self.by_id.setdefault(cell["id"], []).append(cell)
            else:
                self.failed.append(cell)

    def stat(self, cid: str, key: str = "per_run_us") -> Stat | None:
        values = [c[key] for c in self.by_id.get(cid, []) if c.get(key) is not None]
        return Stat(values) if values else None

    def loop(self, framework: str, source: str, variant: str, workload: str) -> Stat | None:
        return self.stat(f"{framework}/{source}/{variant}/{workload}")

    def rounds(self) -> int:
        loops = [c["round"] for c in self.cells if c["phase"] == "loop"]
        return max(loops) if loops else 0

    def frameworks(self) -> list[str]:
        return list(self.plan["frameworks"])

    def label(self, framework: str) -> str:
        return self.plan["frameworks"][framework]["label"]

    def variants_with_role(self, framework: str, role: str) -> list[str]:
        return [v for v, spec in self.plan["frameworks"][framework]["variants"].items() if role in spec["roles"]]

    def sources(self, framework: str) -> list[str]:
        return [s for s in ("head", "release") if any(c["framework"] == framework and c["source"] == s
                                                       for c in self.cells if c["status"] == "ok")]

    def versions(self, framework: str, source: str) -> dict:
        for cell in self.cells:
            if cell["framework"] == framework and cell["source"] == source and cell.get("versions"):
                return cell["versions"]
        return {}


# ── number formatting ────────────────────────────────────────────────────────────────────────────────────────────

def fmt_num(x: float) -> str:
    if x >= 1000:
        return f"{x:,.0f}"
    if x >= 100:
        return f"{x:.0f}"
    if x >= 10:
        return f"{x:.1f}"
    return f"{x:.2f}"


def fmt_stat(s: Stat | None) -> str:
    if s is None:
        return "—"
    if s.n == 1:
        return fmt_num(s.median)
    return f"{fmt_num(s.median)} [{fmt_num(s.low)}, {fmt_num(s.high)}]"


def fmt_ratio(a: Stat | None, b: Stat | None) -> str:
    if a is None or b is None or b.median == 0:
        return "—"
    r = a.median / b.median
    return f"{r:,.0f}×" if r >= 100 else (f"{r:.1f}×" if r >= 10 else f"{r:.2f}×")


def mib(kb: float) -> str:
    return f"{kb / 1024:.1f}"


def md_table(header: list[str], rows: list[list[str]], align: list[str] | None = None) -> str:
    align = align or ["l"] * len(header)
    marks = {"l": ":---", "r": "---:", "c": ":---:"}
    out = ["| " + " | ".join(header) + " |", "| " + " | ".join(marks[a] for a in align) + " |"]
    out += ["| " + " | ".join(row) + " |" for row in rows]
    return "\n".join(out)


# ── version labels ───────────────────────────────────────────────────────────────────────────────────────────────

def parse_requirements(text: str) -> dict:
    """Sections of a requirements file: name -> {"lines": [...], "comments": [...]}."""
    sections: dict = {}
    current = None
    for line in text.splitlines():
        if line.startswith("# venv: "):
            current = line[len("# venv: "):].strip()
            sections[current] = {"lines": [], "comments": []}
        elif current and line.startswith("#"):
            sections[current]["comments"].append(line[1:].strip())
        elif current and line.strip():
            sections[current]["lines"].append(line.strip())
    return sections


def head_label(agg: Agg, framework: str) -> str:
    text = agg.record["provenance"].get("requirements_head", "")
    section = parse_requirements(text).get(f"{framework}-head")
    versions = agg.versions(framework, "head")
    version = version_text(framework, versions)
    if not section:
        return version
    dates = dict(re.findall(r"([\w.-]+/[\w.-]+) (\d{4}-\d\d-\d\d)T[\d:]+Z", " ".join(section["comments"])))
    pieces = []
    for line in section["lines"]:
        m = re.search(r"git\+https://github.com/([^@]+)@([0-9a-f]{40})", line)
        if m:
            repo, sha = m.group(1), m.group(2)[:7]
            if (repo, sha) not in [(p[0], p[1]) for p in pieces]:
                pieces.append((repo, sha, dates.get(repo)))
    commits = ", ".join(f"`{repo.split('/')[-1]}@{sha}`" + (f" ({date})" if date else "") for repo, sha, date in pieces)
    return f"{version}; {commits}" if commits else version


def version_text(framework: str, versions: dict) -> str:
    names = [n for n in versions if versions[n]][:SHOWN_PACKAGES.get(framework, 1)]
    return ", ".join(f"{n} {versions[n]}" for n in names)


def release_label(agg: Agg, framework: str) -> str:
    return version_text(framework, agg.versions(framework, "release"))


def source_label(agg: Agg, framework: str, source: str, locale: str) -> str:
    versions = agg.versions(framework, source)
    ver = next((v for v in versions.values() if v), "?")
    if source == "head":
        text = agg.record["provenance"].get("requirements_head", "")
        section = parse_requirements(text).get(f"{framework}-head")
        sha = ""
        if section:
            main = next(iter(versions), "")
            lines = section["lines"]
            chosen = next((ln for ln in lines if ln.split(" @ ")[0].strip() == main), lines[0] if lines else "")
            m = re.search(r"@([0-9a-f]{40})", chosen)
            if m:
                sha = m.group(1)[:7]
        return f"{tr('head', locale)} `{sha}` ({ver})" if sha else f"{tr('head', locale)} ({ver})"
    return f"{tr('release', locale)} {ver}"


# ── blocks ───────────────────────────────────────────────────────────────────────────────────────────────────────

def ng_stat(agg: Agg, build: str, variant: str, workload: str) -> Stat | None:
    return agg.stat(f"neograph/{build}/{variant}/{workload}")


def order_by_speed(agg: Agg, frameworks: list[str], role: str) -> list[str]:
    def key(fw):
        variants = agg.variants_with_role(fw, role)
        values = [agg.loop(fw, "head", v, "seq") for v in variants]
        values = [v.median for v in values if v]
        return min(values) if values else float("inf")
    return sorted(frameworks, key=key)


def normalized_sha(data: bytes) -> str:
    """sha256 of a script's text with LF line endings, so that a checkout with CRLF endings (core.autocrlf) prints the
    hash that the repository's own files give."""
    import hashlib

    return hashlib.sha256(data.replace(b"\r\n", b"\n")).hexdigest()[:16]


def script_sha() -> str:
    return normalized_sha(Path(__file__).read_bytes())


def block_cohort(agg: Agg, locale: str) -> str:
    rec = agg.record
    prov = rec["provenance"]
    rows = []
    for fw in agg.frameworks():
        rel = release_label(agg, fw) or "—"
        head = head_label(agg, fw) or "—"
        if not agg.sources(fw) or "head" not in agg.sources(fw):
            head += " " + tr("c_fallback", locale)
        rows.append([agg.label(fw), rel, head])
    host = prov["host"]
    session = prov["session"]
    lines = [md_table([tr("c_framework", locale), tr("c_release", locale), tr("c_head", locale)], rows)]
    lines.append("")
    hardened = prov.get("builds", {}).get("hardened", {})
    lines.append(tr("host_line", locale, cpu=host["cpu"]["model"], count=host["cpu"]["count"], kernel=host["kernel"],
                    compiler=hardened.get("compiler_version", "?"),
                    python=prov["packages"][next(iter(prov["packages"]))]["python"] if prov.get("packages") else "?",
                    governor=tr("governor_na", locale) if host["cpu"]["governor"] == "not exposed by the host"
                    else host["cpu"]["governor"]))
    lines.append("")
    phases = session.get("phases") or []
    load0 = (phases[0]["loadavg_start"] if phases else session["loadavg_start"])[0]
    load1 = session["loadavg_end"][0]
    lines.append(tr("session_line", locale, start=session["utc_start"], end=session["utc_end"],
                    before=f"{load0:.2f}", after=f"{load1:.2f}", rounds=agg.rounds(),
                    record=prov.get("record_name", "")))
    lines.append("")
    lines.append(tr("engine_line", locale, commit=prov["neograph_engine_commit"][:12],
                    harness=prov["harness_commit"][:12]))
    lines.append("")
    lines.append(tr("render_line", locale, sha=script_sha()))
    return "\n".join(lines)


def matched_header(locale: str) -> list[str]:
    return [tr("framework", locale), tr("source", locale), tr("seq", locale), tr("par", locale),
            tr("seq_ratio", locale), tr("par_ratio", locale)]


def block_matched_single(agg: Agg, locale: str) -> str:
    ng_seq = ng_stat(agg, "hardened", "workers_1", "seq")
    ng_par = ng_stat(agg, "hardened", "workers_1", "par")
    rows = [[f"**{tr('ng_w1', locale)}**", "", f"**{fmt_stat(ng_seq)}**", f"**{fmt_stat(ng_par)}**", "1×", "1×"]]
    for fw in order_by_speed(agg, agg.frameworks(), "matched_single"):
        variant = (agg.variants_with_role(fw, "matched_single") or [None])[0]
        if variant is None:
            continue
        vlabel = variant_label(agg, fw, variant, locale)
        for source in ("head", "release"):
            seq = agg.loop(fw, source, variant, "seq")
            par = agg.loop(fw, source, variant, "par")
            if seq is None and par is None:
                continue
            rows.append([f"{agg.label(fw)} — {vlabel}", source_label(agg, fw, source, locale), fmt_stat(seq),
                         fmt_stat(par), fmt_ratio(seq, ng_seq), fmt_ratio(par, ng_par)])
    return "\n".join([md_table(matched_header(locale), rows, ["l", "l", "r", "r", "r", "r"]), "",
                      tr("cell_line", locale, rounds=agg.rounds())])


def block_matched_pool(agg: Agg, locale: str) -> str:
    ng_seq = ng_stat(agg, "hardened", "workers_auto", "seq")
    ng_par = ng_stat(agg, "hardened", "workers_auto", "par")
    ng_w1_par = ng_stat(agg, "hardened", "workers_1", "par")
    workers = ""
    for cell in agg.by_id.get("neograph/hardened/workers_auto/par", []):
        workers = str(cell.get("workers") or "")
    rows = [[f"**{tr('ng_auto', locale)}**", "", f"**{fmt_stat(ng_seq)}**", f"**{fmt_stat(ng_par)}**", "1×", "1×"]]
    for fw in order_by_speed(agg, agg.frameworks(), "matched_single"):
        variants = agg.variants_with_role(fw, "matched_pool")
        if not variants:
            reason = tr(f"pool_na_{fw}", locale) if f"pool_na_{fw}" in T else ""
            rows.append([agg.label(fw), "", tr("na_sep", locale) + reason, "", "", ""])
            continue
        variant = variants[0]
        vlabel = variant_label(agg, fw, variant, locale)
        for source in ("head", "release"):
            seq = agg.loop(fw, source, variant, "seq")
            par = agg.loop(fw, source, variant, "par")
            if seq is None and par is None:
                continue
            rows.append([f"{agg.label(fw)} — {vlabel}", source_label(agg, fw, source, locale), fmt_stat(seq),
                         fmt_stat(par), fmt_ratio(seq, ng_seq), fmt_ratio(par, ng_par)])
    out = [md_table(matched_header(locale), rows, ["l", "l", "r", "r", "r", "r"])]
    if ng_w1_par and ng_par:
        out += ["", tr("range_line", locale, w1=fmt_num(ng_w1_par.median), auto=fmt_num(ng_par.median),
                       workers=workers.replace("auto(", "").replace(")", "") or "?")]
    return "\n".join(out)


def block_idiomatic(agg: Agg, locale: str) -> str:
    header = [tr("framework", locale), tr("api", locale), tr("seq", locale), tr("par", locale)]
    ng_seq = ng_stat(agg, "hardened", "workers_1", "seq")
    ng_par = ng_stat(agg, "hardened", "workers_1", "par")
    rows = [[f"**NeoGraph**", f"**{tr('ng_default', locale)}**", f"**{fmt_stat(ng_seq)}**", f"**{fmt_stat(ng_par)}**"]]
    for fw in order_by_speed(agg, agg.frameworks(), "matched_single"):
        for variant in agg.variants_with_role(fw, "idiomatic"):
            seq = agg.loop(fw, "head", variant, "seq")
            par = agg.loop(fw, "head", variant, "par")
            if seq is None and par is None:
                continue
            vlabel = variant_label(agg, fw, variant, locale)
            rows.append([agg.label(fw), vlabel, fmt_stat(seq), fmt_stat(par)])
    return md_table(header, rows, ["l", "l", "r", "r"])


def block_ceiling(agg: Agg, locale: str) -> str:
    header = [tr("ceiling_variant", locale), tr("ng_row_w1_seq", locale), tr("ng_row_w1_par", locale),
              tr("ng_row_auto_par", locale)]
    rows = []
    stats = {}
    for build in ("hardened", "unhardened"):
        stats[build] = (ng_stat(agg, build, "workers_1", "seq"), ng_stat(agg, build, "workers_1", "par"),
                        ng_stat(agg, build, "workers_auto", "par"))
        rows.append([tr(f"ceiling_{build}", locale)] + [fmt_stat(s) for s in stats[build]])
    h, u = stats["hardened"], stats["unhardened"]
    rows.append([tr("ceiling_ratio", locale)] + [fmt_ratio(h[i], u[i]) for i in range(3)])
    out = [md_table(header, rows, ["l", "r", "r", "r"])]
    events = ng_stat(agg, "hardened", "workers_1_events", "seq")
    if events:
        out += ["", md_table([tr("ceiling_variant", locale), "µs"],
                             [[tr("events_row", locale), fmt_stat(events)]], ["l", "r"])]
    builds = agg.record["provenance"].get("builds", {})
    flags = []
    for build in ("hardened", "unhardened"):
        info = builds.get(build)
        if not info:
            continue
        flags.append(f"[{build}] build type: {info.get('cmake_build_type', '?')}; "
                     f"{info.get('compiler_version', '')}")
        for key in ("cmake_cxx_flags", "neograph_enable_hardening"):
            if info.get(key) not in (None, ""):
                flags.append(f"[{build}] {key.upper()}: {info[key]}")
        cmd = info.get("bench_neograph_compile_command")
        if cmd:
            flags.append(f"[{build}] compile: {cmd}")
        facts = None
        for cell in agg.by_id.get(f"neograph/{build}/workers_1/seq", []):
            facts = cell.get("build")
        if facts:
            flags.append(f"[{build}] macros seen by the binary: _GLIBCXX_ASSERTIONS={facts.get('glibcxx_assertions')}, "
                         f"_FORTIFY_SOURCE={facts.get('fortify_source')}, stack protector={facts.get('stack_protector')}, "
                         f"PIE={facts.get('pie')}")
    if flags:
        out += ["", tr("flags_title", locale), "", "```text"] + flags + ["```"]
    return "\n".join(out)


def peak_stat(agg: Agg, fw: str, workload: str) -> Stat | None:
    variant = (agg.variants_with_role(fw, "matched_single") or [None])[0]
    if variant is None:
        return None
    values = [c["process"]["hwm_kb_end"] for c in agg.by_id.get(f"{fw}/head/{variant}/{workload}", [])
              if c.get("process")]
    return Stat(values) if values else None


def idle_stat(agg: Agg, cid: str, section: str, key: str = "vm_rss_kb") -> Stat | None:
    values = [c[section][key] for c in agg.by_id.get(cid, []) if c.get(section)]
    return Stat(values) if values else None


def marginal(agg: Agg, cid_prefix: str, key: str) -> float | None:
    """(plateau at N=64 - plateau at N=1) / 63, median over rounds (the largest N when 64 was not run)."""
    ns = sorted({c["n"] for cid, cells in agg.by_id.items() if cid.startswith(cid_prefix + "/") for c in cells
                 if c.get("n")})
    if len(ns) < 2:
        return None
    lo, hi = ns[0], (64 if 64 in ns else ns[-1])
    per_round: dict[int, dict] = {}
    for cid, cells in agg.by_id.items():
        if not cid.startswith(cid_prefix + "/"):
            continue
        for c in cells:
            if c.get("n") in (lo, hi) and c.get("plateau"):
                per_round.setdefault(c["round"], {})[c["n"]] = c["plateau"][key]
    deltas = [(v[hi] - v[lo]) / (hi - lo) for v in per_round.values() if lo in v and hi in v]
    return statistics.median(deltas) if deltas else None


def block_memory(agg: Agg, locale: str) -> str:
    header = [tr("mem_framework", locale), tr("mem_idle", locale), tr("mem_peak_seq", locale),
              tr("mem_peak_par", locale), tr("mem_inc", locale)]
    rows = []
    py = idle_stat(agg, "python/bare/idle", "after_import")
    rows.append([tr("mem_baseline_python", locale), mib(py.median) if py else "—", "—", "—", "—"])
    ng_idle = idle_stat(agg, "neograph/hardened/idle", "after_compile")
    ng_seq_peak = Stat([c["process"]["hwm_kb_end"] for c in agg.by_id.get("neograph/hardened/workers_1/seq", [])
                        if c.get("process")] or [0])
    ng_par_peak = Stat([c["process"]["hwm_kb_end"] for c in agg.by_id.get("neograph/hardened/workers_1/par", [])
                        if c.get("process")] or [0])
    rows.append([tr("mem_ng_row", locale),
                 mib(ng_idle.median) if ng_idle else "—", mib(ng_seq_peak.median), mib(ng_par_peak.median),
                 (f"{mib(ng_seq_peak.median - ng_idle.median)} / {mib(ng_par_peak.median - ng_idle.median)}"
                  if ng_idle else "—")])
    for fw in order_by_speed(agg, agg.frameworks(), "matched_single"):
        idle = idle_stat(agg, f"{fw}/head/idle", "after_import")
        ps, pp = peak_stat(agg, fw, "seq"), peak_stat(agg, fw, "par")
        if not idle:
            continue
        inc = (f"{mib(ps.median - idle.median)} / {mib(pp.median - idle.median)}" if ps and pp else "—")
        rows.append([agg.label(fw), mib(idle.median), mib(ps.median) if ps else "—", mib(pp.median) if pp else "—", inc])
    out = [md_table(header, rows, ["l", "r", "r", "r", "r"]), ""]
    header2 = [tr("mem_framework", locale), tr("mem_marginal_cols", locale), tr("mem_marginal_cols_par", locale)]
    rows2 = []

    def pair(prefix):
        rss, pss = marginal(agg, prefix, "vm_rss_kb"), marginal(agg, prefix, "pss_kb")
        return f"{rss:.1f} / {pss:.1f}" if rss is not None and pss is not None else "—"

    rows2.append(["**NeoGraph**", pair("neograph/hardened/inflight/seq"), pair("neograph/hardened/inflight/par")])
    for fw in order_by_speed(agg, agg.frameworks(), "matched_single"):
        variant = (agg.variants_with_role(fw, "matched_single") or [None])[0]
        if variant is None:
            continue
        rows2.append([agg.label(fw), pair(f"{fw}/head/{variant}/seq/inflight"), pair(f"{fw}/head/{variant}/par/inflight")])
    out.append(f"**{tr('mem_marginal_title', locale)}**")
    out.append("")
    out.append(md_table(header2, rows2, ["l", "r", "r"]))
    return "\n".join(out)


def block_footprint(agg: Agg, locale: str) -> str:
    out = []
    header = [tr("cold_framework", locale), tr("cold_total", locale), tr("cold_parts", locale)]
    rows = []
    ng = agg.stat("neograph/hardened/cold", "cold_start_ms")
    if ng:
        parts = "— / " + " / ".join(fmt_num(agg_median(agg, "neograph/hardened/cold", k) / 1000)
                                    for k in ("compile_us", "first_run_us"))
        rows.append(["**NeoGraph**", f"**{fmt_stat(ng)}**", parts])
    for fw in order_by_speed(agg, agg.frameworks(), "matched_single"):
        variant = (agg.variants_with_role(fw, "matched_single") or [None])[0]
        cid = f"{fw}/head/{variant}/seq/cold"
        s = agg.stat(cid, "cold_start_ms")
        if not s:
            continue
        parts = " / ".join(fmt_num(agg_median(agg, cid, k) / 1000) for k in ("import_us", "build_us", "first_run_us"))
        rows.append([agg.label(fw), fmt_stat(s), parts])
    out.append(md_table(header, rows, ["l", "r", "r"]))
    out.append("")
    sizes = agg.record.get("sizes", {})
    srows = []

    def mb(b):
        return f"{b / 1e6:.2f} MB" if b is not None else "—"

    for config in ("hardened", "minsize", "unhardened"):
        label = tr(f"cfg_{config}", locale)
        entry = sizes.get("neograph", {}).get(config)
        if not entry:
            continue
        for name, what in (("bench_footprint", "note_minapp"), ("bench_neograph", "note_benchbin")):
            if name in entry and entry[name].get("stripped_bytes"):
                srows.append([tr("item_ng_lib", locale, name=name, cfg=label), tr("unit_binary", locale),
                              mb(entry[name]["stripped_bytes"]), tr(what, locale)])
        for name in ("libneograph_core.a", "libneograph_core.so"):
            # An archive built with link-time optimisation holds compiler bitcode, not code: its size is not a
            # deployable size, so the table leaves it out.
            if name in entry and not (config == "minsize" and name.endswith(".a")):
                v = entry[name].get("stripped_bytes") or entry[name].get("as_built_bytes")
                srows.append([tr("item_ng_lib", locale, name=name, cfg=label), tr("unit_library", locale), mb(v),
                              tr("note_stripped" if name.endswith(".so") else "note_archive", locale)])
    shared = sizes.get("neograph", {}).get("shared")
    if shared and "bench_footprint" in shared:
        exe = shared["bench_footprint"]
        own = exe.get("own_shared_libraries_stripped_bytes") or {}
        srows.append([tr("item_shared_exe", locale), tr("unit_binary", locale), mb(exe.get("stripped_bytes")),
                      tr("note_exe_only", locale)])
        if own:
            srows.append([tr("item_shared_deploy", locale), tr("unit_shared_set", locale),
                          mb((exe.get("stripped_bytes") or 0) + sum(v or 0 for v in own.values())),
                          tr("note_shared_deploy", locale, n=len(own))])
    wasm = agg.record.get("wasm")
    if wasm:
        for item in wasm.get("modules", []):
            srows.append([tr(f"wasm_{item['kind']}", locale) + f" ({wasm.get('toolchain', '')})",
                          tr("unit_wasm", locale),
                          f"{item['raw_bytes'] / 1e6:.2f} MB / {item['gzip_bytes'] / 1e6:.2f} MB gzip",
                          tr("note_wasm", locale, commit=wasm.get("commit", ""))])
    for fw in order_by_speed(agg, agg.frameworks(), "matched_single"):
        entry = sizes.get("python", {}).get(f"{fw}-head")
        if entry and entry.get("installed_bytes") is not None:
            srows.append([tr("item_head_install", locale, fw=agg.label(fw)), tr("unit_venv", locale),
                          mb(entry["installed_bytes"]), tr("note_venv", locale, n=entry["packages"])])
    out.append(md_table([tr("size_item", locale), tr("size_unit", locale), tr("size_value", locale),
                         tr("size_note", locale)], srows, ["l", "l", "r", "l"]))
    return "\n".join(out)


def agg_median(agg: Agg, cid: str, key: str) -> float:
    values = [c[key] for c in agg.by_id.get(cid, []) if c.get(key) is not None]
    return statistics.median(values) if values else 0.0


def block_probes(agg: Agg, locale: str) -> str:
    probes = agg.record.get("probes", {})
    rows = []

    def add(name, text):
        rows.append([name, text])

    li = probes.get("llamaindex_reuse")
    if li:
        res = li["result"]
        detail = tr("reuse_detail", locale, seq=res["sequential_runs"], conc=res["concurrent_runs"],
                    version=res["versions"].get("llama-index-workflows"))
        add("1", tr("llamaindex_reuse" if res["reusable"] else "llamaindex_reuse_no", locale, detail=detail))
        reused_seq = agg.loop("llamaindex", "head", "reused_instance", "seq")
        fresh_seq = agg.loop("llamaindex", "head", "fresh_instance", "seq")
        reused_par = agg.loop("llamaindex", "head", "reused_instance", "par")
        fresh_par = agg.loop("llamaindex", "head", "fresh_instance", "par")
        if all((reused_seq, fresh_seq, reused_par, fresh_par)):
            add("1", tr("reuse_timing", locale, reused_seq=fmt_num(reused_seq.median), fresh_seq=fmt_num(fresh_seq.median),
                        reused_par=fmt_num(reused_par.median), fresh_par=fmt_num(fresh_par.median)))
    th = probes.get("threads_langgraph")
    if th:
        res = th["result"]
        a_seq, a_par = agg.loop("langgraph", "head", "ainvoke_async", "seq"), agg.loop("langgraph", "head", "ainvoke_async", "par")
        i_seq, i_par = agg.loop("langgraph", "head", "invoke_sync", "seq"), agg.loop("langgraph", "head", "invoke_sync", "par")
        if all((a_seq, a_par, i_seq, i_par)):
            add("2", tr("langgraph_threads", locale, inv=res["invoke_sync"]["worker_thread_count_excluding_main"],
                        inv_par=fmt_num(i_par.median), ainv_par=fmt_num(a_par.median),
                        par_ratio=f"{i_par.median / a_par.median:.2f}", inv_seq=fmt_num(i_seq.median),
                        ainv_seq=fmt_num(a_seq.median), seq_ratio=f"{i_seq.median / a_seq.median:.2f}"))
    hs = probes.get("threads_haystack")
    if hs:
        add("2", tr("haystack_threads", locale, pool=hs["result"]["run_async_pool"]["worker_thread_count_excluding_main"]))
    pa = probes.get("pydantic_async")
    if pa:
        res = pa["result"]
        err = res["sync_step_function"].get("error", "accepted") if not res["sync_step_function"]["accepted"] else "accepted"
        add("3", tr("pydantic_async", locale, version=res["version"], sync_error=err,
                    elapsed=f"{res['fan_out']['elapsed_s'] * 1000:.0f}", threads=res["fan_out"]["distinct_threads"]))
    h = [ng_stat(agg, "hardened", "workers_1", "seq"), ng_stat(agg, "hardened", "workers_1", "par"),
         ng_stat(agg, "hardened", "workers_auto", "par")]
    u = [ng_stat(agg, "unhardened", "workers_1", "seq"), ng_stat(agg, "unhardened", "workers_1", "par"),
         ng_stat(agg, "unhardened", "workers_auto", "par")]
    if all(h) and all(u):
        r = [f"{h[i].median / u[i].median:.2f}" for i in range(3)]
        add("4", tr("hardening", locale, seq_ratio=r[0], par_ratio=r[1], auto_ratio=r[2]))
    pl = probes.get("pydantic_legacy")
    if pl:
        m = pl["result"]["median_us_per_run"]
        add("5", tr("pydantic_legacy", locale, loop=fmt_num(m["loop"]), burst=fmt_num(m["burst"]),
                    task=fmt_num(m["loop_in_task"]), gc=fmt_num(m["loop_gc_off"])))
    plf = probes.get("pydantic_legacy_logfire")
    if pl and plf:
        clean, lf = pl["result"]["median_us_per_run"]["loop"], plf["result"]["median_us_per_run"]["loop"]
        add("5", tr("pydantic_legacy_logfire", locale, lf=fmt_num(lf), clean=fmt_num(clean), ratio=f"{lf / clean:.1f}"))
    out = [md_table(["#", tr("probe_result", locale)], rows, ["l", "l"])] if rows else []
    if agg.failed:
        failed = {}
        for cell in agg.failed:
            failed.setdefault(cell["id"], []).append(cell)
        frows = [[cid, str(len(cells)), cells[0].get("first_error") or ""] for cid, cells in sorted(failed.items())]
        out += ["", f"**{tr('failed_cells', locale)}**", "",
                md_table(["cell", "n", "error"], frows)]
    return "\n".join(out)


def concurrent_rows(agg: Agg) -> list[dict]:
    return getattr(agg, "concurrent", [])


ENGINE_LABELS = {"neograph": "NeoGraph", "langgraph": "LangGraph", "haystack": "Haystack",
                 "pydantic-graph": "pydantic-graph", "llamaindex": "LlamaIndex", "autogen": "AutoGen"}


def wall_seconds(row: dict) -> float | None:
    if row.get("total_wall_us") is not None:
        return row["total_wall_us"] / 1e6
    if row.get("total_wall_ms") is not None:
        return row["total_wall_ms"] / 1e3
    return None


def fmt_wall(seconds: float) -> str:
    return f"{seconds * 1000:.0f} ms" if seconds < 1 else f"{seconds:.1f} s"


def concurrent_valid(row: dict) -> bool:
    """A row is a result only when every call completed. A row with no completion measured an exception."""
    return row.get("status") == "ok" and row.get("ok", 0) > 0 and row.get("err", 0) == 0


def block_concurrent_wall(agg: Agg, locale: str) -> str:
    rows = [r for r in concurrent_rows(agg) if r.get("profile") == "1:512m" and r.get("concurrency") == 10000]
    header = [tr("k_n", locale), tr("k_engine", locale), tr("k_wall", locale), tr("k_rss", locale), tr("k_ok", locale)]
    out = []
    for row in sorted(rows, key=lambda r: (r["engine"] != "neograph", wall_seconds(r) or 1e9)):
        label = tr("k_mode", locale, engine=ENGINE_LABELS.get(row["engine"], row["engine"]), mode=row["mode"])
        if row.get("status") == "oom_killed":
            out.append(["10,000", label, tr("k_oom", locale), "—", "—"])
            continue
        if not concurrent_valid(row):
            continue  # a row that measured an exception is not a result
        multiproc = row["mode"].startswith("mp-")
        rss = tr("k_withdrawn", locale) if multiproc else mib(row["peak_rss_kb"])
        out.append(["10,000", label, fmt_wall(wall_seconds(row)), rss, f"{row['ok']} / {row['err']}"])
    return md_table(header, out, ["r", "l", "r", "r", "r"])


def chart_lines(rows: list[dict], value, title: str, ylabel: str, log_y: bool, min_wall_s: float | None) -> str:
    import math

    series: dict[str, list[tuple[int, float]]] = {}
    for row in rows:
        if row.get("profile") != "1:512m" or not concurrent_valid(row) or row["mode"].startswith("mp-"):
            continue
        wall = wall_seconds(row)
        if wall is None or (min_wall_s is not None and wall < min_wall_s):
            continue
        v = value(row, wall)
        if v is None:
            continue
        key = f"{ENGINE_LABELS.get(row['engine'], row['engine'])} {row['mode']}"
        series.setdefault(key, []).append((row["concurrency"], v))
    width, height, left, right, top, bottom = 760, 420, 78, 150, 60, 54
    xs = sorted({n for pts in series.values() for n, _ in pts}) or [10, 10000]
    ys = [v for pts in series.values() for _, v in pts] or [1.0]
    y0, y1 = 10 ** math.floor(math.log10(min(ys))), 10 ** math.ceil(math.log10(max(ys)))
    x0, x1 = math.log10(xs[0]), math.log10(xs[-1])

    def px(n):
        return left + (math.log10(n) - x0) / ((x1 - x0) or 1) * (width - left - right)

    def py(v):
        return height - bottom - (math.log10(v) - math.log10(y0)) / ((math.log10(y1) - math.log10(y0)) or 1) * (height - top - bottom)

    colors = {"NeoGraph": "#2ea44f", "LangGraph": "#8250df", "Haystack": "#1f77b4", "pydantic-graph": "#17a2b8",
              "LlamaIndex": "#d3822a", "AutoGen": "#c0392b"}
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" width="{width}" height="{height}" '
             f'role="img" aria-label="{esc(title)}" font-family="system-ui, -apple-system, Segoe UI, Roboto, sans-serif">',
             f'<rect width="{width}" height="{height}" fill="#ffffff"/>',
             f'<text x="{width / 2}" y="26" text-anchor="middle" font-size="15" font-weight="700" fill="#24292e">{esc(title)}</text>',
             f'<text x="{width / 2}" y="44" text-anchor="middle" font-size="11" fill="#586069">closed single burst, 1 CPU / 512 MiB; points whose wall time is below the clock resolution are left out</text>']
    d = int(math.log10(y0))
    while 10 ** d <= y1:
        v = 10 ** d
        parts.append(f'<line x1="{left}" x2="{width - right}" y1="{py(v):.1f}" y2="{py(v):.1f}" stroke="#eaecef"/>')
        parts.append(f'<text x="{left - 6}" y="{py(v) + 4:.1f}" text-anchor="end" font-size="10.5" fill="#586069">{v:,.0f}</text>')
        d += 1
    for n in xs:
        parts.append(f'<text x="{px(n):.1f}" y="{height - bottom + 16}" text-anchor="middle" font-size="10.5" fill="#586069">{n:,}</text>')
    parts.append(f'<text x="{(left + width - right) / 2}" y="{height - 12}" text-anchor="middle" font-size="11" fill="#586069">N, simultaneous runs submitted</text>')
    parts.append(f'<text x="16" y="{(top + height - bottom) / 2}" text-anchor="middle" font-size="11" fill="#586069" transform="rotate(-90 16 {(top + height - bottom) / 2})">{esc(ylabel)}</text>')
    for k, (key, pts) in enumerate(sorted(series.items())):
        color = colors.get(key.split(" ")[0], "#666666")
        pts = sorted(pts)
        path = " ".join(f"{px(n):.1f},{py(v):.1f}" for n, v in pts)
        parts.append(f'<polyline points="{path}" fill="none" stroke="{color}" stroke-width="2"/>')
        for n, v in pts:
            parts.append(f'<circle cx="{px(n):.1f}" cy="{py(v):.1f}" r="3" fill="{color}"/>')
        parts.append(f'<rect x="{width - right + 12}" y="{top + 8 + 18 * k}" width="10" height="10" fill="{color}"/>'
                     f'<text x="{width - right + 28}" y="{top + 17 + 18 * k}" font-size="11" fill="#24292e">{esc(key)}</text>')
    parts.append("</svg>")
    return "\n".join(parts) + "\n"


def block_dr_compare(agg: Agg, locale: str) -> str:
    dr = getattr(agg, "dr", None)
    if dr is None:
        return ""
    plan = dr["plan"]
    summary = dr["summary"]
    rows = []
    for name, config in plan["configs"].items():
        entry = summary.get(name)
        if not entry:
            continue

        def fmt(side):
            e = entry.get(side)
            if not e:
                return "—"
            ms = lambda v: fmt_num(v * 1000)  # noqa: E731
            return f"{ms(e['median_s'])} [{ms(e['min_s'])}, {ms(e['max_s'])}]"

        ratio = entry.get("ratio_langgraph_over_neograph")
        rows.append([tr(f"dr_{name}", locale), str(config["fanout"]), str(config["mock_ms"]), str(config["width"]),
                     fmt("neograph"), fmt("langgraph"), f"{ratio:.2f}×" if ratio else "—"])
    header = [tr("dr_cfg", locale), tr("dr_fanout", locale), tr("dr_delay", locale), tr("dr_width", locale),
              tr("dr_ng", locale), tr("dr_lg", locale), tr("dr_ratio", locale)]
    groups: dict[int, list[str]] = {}
    for name, config in plan["configs"].items():
        groups.setdefault(config["iters"], []).append(name)
    iters_text = "; ".join(f"{n} ({', '.join(names)})" for n, names in groups.items())
    module = dr["provenance"].get("neograph_module", {})
    prov = dr["provenance"]
    out = [md_table(header, rows, ["l", "r", "r", "r", "r", "r", "r"]), "",
           tr("dr_line", locale, rounds=plan["rounds"], warmup=plan["warmup"], iters=iters_text,
              version=module.get("version", "?"), compiler=(module.get("build") or {}).get("compiler_version", "?")),
           "", tr("host_line", locale, cpu=prov["host"]["cpu"]["model"], count=prov["host"]["cpu"]["count"],
                  kernel=prov["host"]["kernel"], python=prov["python"]["version"],
                  compiler=(module.get("build") or {}).get("compiler_version", "?"),
                  governor=tr("governor_na", locale) if prov["host"]["cpu"]["governor"] == "not exposed by the host"
                  else prov["host"]["cpu"]["governor"])]
    return "\n".join(out)


BLOCKS = {
    "dr_compare": block_dr_compare,
    "concurrent_wall": block_concurrent_wall,
    "cohort": block_cohort,
    "matched_single": block_matched_single,
    "matched_pool": block_matched_pool,
    "idiomatic": block_idiomatic,
    "ceiling": block_ceiling,
    "memory": block_memory,
    "footprint": block_footprint,
    "probes": block_probes,
}


# ── chart ────────────────────────────────────────────────────────────────────────────────────────────────────────

PALETTE = {"neograph": "#2ea44f", "haystack": "#1f77b4", "pydantic_graph": "#17a2b8", "langgraph": "#8250df",
           "llamaindex": "#d3822a", "autogen": "#c0392b"}


def esc(text: str) -> str:
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def render_svg(agg: Agg) -> str:
    import math

    ng_seq = ng_stat(agg, "hardened", "workers_1", "seq")
    ng_par = ng_stat(agg, "hardened", "workers_1", "par")
    entries = [("NeoGraph", "neograph", ng_seq, ng_par)]
    for fw in order_by_speed(agg, agg.frameworks(), "matched_single"):
        variant = (agg.variants_with_role(fw, "matched_single") or [None])[0]
        if variant:
            entries.append((agg.label(fw), fw, agg.loop(fw, "head", variant, "seq"), agg.loop(fw, "head", variant, "par")))
    entries = [e for e in entries if e[2] and e[3]]
    values = [v.high for e in entries for v in (e[2], e[3])]
    top = 10 ** math.ceil(math.log10(max(values)))
    bottom = 10 ** math.floor(math.log10(min(v.low for e in entries for v in (e[2], e[3])) * 0.9))
    width, height = 920, 520
    left, right, plot_top, plot_bottom = 84, 24, 96, 392
    plot_w = width - left - right
    span = math.log10(top) - math.log10(bottom)

    def y_of(v: float) -> float:
        return plot_bottom - (math.log10(v) - math.log10(bottom)) / span * (plot_bottom - plot_top)

    session = agg.record["provenance"]["session"]
    host = agg.record["provenance"]["host"]
    title = "Matched regime, single thread: engine overhead per run (µs, log scale, lower is better)"
    sub = (f"NeoGraph worker_count=1 against each framework's coroutine path (async def nodes); HEAD builds; "
           f"median of {agg.rounds()} interleaved rounds, whiskers show [min, max] across rounds")
    foot = (f"{host['cpu']['model']}, {host['cpu']['count']} cores, kernel {host['kernel']}; "
            f"{session['utc_start'][:10]}; record {agg.record['provenance'].get('record_name', '')}. "
            f"No I/O, no model calls; checkpointing off. seq = 3-node chain, par = fan-out 5 + join.")
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" width="{width}" height="{height}" '
             f'role="img" aria-labelledby="t d" font-family="system-ui, -apple-system, Segoe UI, Roboto, sans-serif">',
             f'<title id="t">{esc(title)}</title>',
             f'<desc id="d">{esc(sub)}</desc>',
             f'<rect width="{width}" height="{height}" fill="#ffffff"/>',
             f'<text x="{width / 2}" y="30" text-anchor="middle" font-size="16" font-weight="700" fill="#24292e">{esc(title)}</text>',
             f'<text x="{width / 2}" y="52" text-anchor="middle" font-size="11" fill="#586069">{esc(sub)}</text>']
    decade = math.floor(math.log10(bottom))
    while 10 ** decade <= top:
        v = 10 ** decade
        y = y_of(v)
        parts.append(f'<line x1="{left}" x2="{width - right}" y1="{y:.1f}" y2="{y:.1f}" stroke="#eaecef"/>')
        parts.append(f'<text x="{left - 8}" y="{y + 4:.1f}" text-anchor="end" font-size="11" fill="#586069">{v:,.0f}</text>')
        decade += 1
    group_w = plot_w / len(entries)
    bar_w = min(34, group_w / 2.6)
    for i, (label, key, seq, par) in enumerate(entries):
        cx = left + group_w * (i + 0.5)
        color = PALETTE.get(key, "#666666")
        for j, (stat, shade) in enumerate(((seq, 1.0), (par, 0.55))):
            x = cx + (j - 1) * bar_w + 3 * (2 * j - 1) - bar_w * (0 if j else 0)
            x = cx - bar_w - 2 + j * (bar_w + 4)
            y = y_of(stat.median)
            parts.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{bar_w:.1f}" height="{plot_bottom - y:.1f}" fill="{color}" '
                         f'fill-opacity="{shade}" rx="3"/>')
            yl, yh = y_of(stat.low), y_of(stat.high)
            mx = x + bar_w / 2
            parts.append(f'<line x1="{mx:.1f}" x2="{mx:.1f}" y1="{yl:.1f}" y2="{yh:.1f}" stroke="#24292e" stroke-width="1.2"/>')
            parts.append(f'<line x1="{mx - 4:.1f}" x2="{mx + 4:.1f}" y1="{yl:.1f}" y2="{yl:.1f}" stroke="#24292e"/>')
            parts.append(f'<line x1="{mx - 4:.1f}" x2="{mx + 4:.1f}" y1="{yh:.1f}" y2="{yh:.1f}" stroke="#24292e"/>')
            parts.append(f'<text x="{mx:.1f}" y="{yh - 6:.1f}" text-anchor="middle" font-size="10.5" fill="#24292e">{fmt_num(stat.median)}</text>')
        parts.append(f'<text x="{cx:.1f}" y="{plot_bottom + 18}" text-anchor="middle" font-size="12" fill="#24292e">{esc(label)}</text>')
    parts.append(f'<line x1="{left}" x2="{left}" y1="{plot_top}" y2="{plot_bottom}" stroke="#d1d5da"/>')
    parts.append(f'<line x1="{left}" x2="{width - right}" y1="{plot_bottom}" y2="{plot_bottom}" stroke="#d1d5da"/>')
    parts.append(f'<rect x="{left}" y="{plot_bottom + 38}" width="12" height="12" fill="#586069"/>'
                 f'<text x="{left + 18}" y="{plot_bottom + 49}" font-size="11" fill="#24292e">seq (solid)</text>'
                 f'<rect x="{left + 110}" y="{plot_bottom + 38}" width="12" height="12" fill="#586069" fill-opacity="0.55"/>'
                 f'<text x="{left + 128}" y="{plot_bottom + 49}" font-size="11" fill="#24292e">par (lighter)</text>')
    for k, line in enumerate(wrap(foot, 150)):
        parts.append(f'<text x="{width / 2}" y="{plot_bottom + 78 + 14 * k}" text-anchor="middle" font-size="10.5" fill="#586069">{esc(line)}</text>')
    parts.append("</svg>")
    return "\n".join(parts) + "\n"


def wrap(text: str, width: int) -> list[str]:
    words, lines, current = text.split(), [], ""
    for word in words:
        if len(current) + len(word) + 1 > width and current:
            lines.append(current)
            current = word
        else:
            current = f"{current} {word}".strip()
    if current:
        lines.append(current)
    return lines


# ── documents ────────────────────────────────────────────────────────────────────────────────────────────────────

MARK = re.compile(r"(<!-- bench-record:begin (\w+) -->)(.*?)(<!-- bench-record:end \2 -->)", re.DOTALL)


def doc_path(locale: str, base: str = README) -> Path:
    suffix = "" if locale == "en" else f".{locale}"
    return ROOT / f"{base}{suffix}.md"


def render_doc(path: Path, agg: Agg, locale: str) -> str:
    text = path.read_text(encoding="utf-8")

    def replace(match: re.Match) -> str:
        name = match.group(2)
        if name not in BLOCKS:
            raise SystemExit(f"{path}: unknown block {name!r}")
        return match.group(1) + "\n" + BLOCKS[name](agg, locale) + "\n" + match.group(4)

    return MARK.sub(replace, text)


def current_record_path(arg: str | None) -> Path:
    if arg:
        return Path(arg)
    pointer = RECORDS / "CURRENT"
    return RECORDS / pointer.read_text(encoding="utf-8").strip()


def validate_record(record: dict) -> None:
    if record.get("schema") != 1 or record.get("kind") != "cross_framework_cohort":
        raise SystemExit("not a cross-framework cohort record (schema 1)")
    prov = record.get("provenance", {})
    for key in ("harness_commit", "neograph_engine_commit", "script_sha256", "session", "host", "builds", "packages"):
        if not prov.get(key):
            raise SystemExit(f"record lacks provenance.{key}")
    for key in ("utc_start", "utc_end", "loadavg_start", "loadavg_end"):
        if key not in prov["session"]:
            raise SystemExit(f"record lacks provenance.session.{key}")
    host = prov["host"]
    if not host.get("kernel") or not host.get("cpu", {}).get("model"):
        raise SystemExit("record lacks the host CPU model or kernel")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--record", help="record file (default: the one named in benchmarks/records/CURRENT)")
    parser.add_argument("--check", action="store_true", help="exit 1 when a rendered file differs from the disk")
    parser.add_argument("--docs", nargs="*", help="document bases to render (default: benchmarks/README)")
    parser.add_argument("--dr-record", help="dr_compare session record (default: benchmarks/records/CURRENT_DR_COMPARE)")
    args = parser.parse_args()

    path = current_record_path(args.record)
    record = json.loads(path.read_text(encoding="utf-8"))
    validate_record(record)
    record["provenance"]["record_name"] = path.name
    agg = Agg(record)

    dr_pointer = RECORDS / "CURRENT_DR_COMPARE"
    dr_path = Path(args.dr_record) if args.dr_record else (RECORDS / dr_pointer.read_text(encoding="utf-8").strip()
                                                           if dr_pointer.exists() else None)
    agg.dr = json.loads(dr_path.read_text(encoding="utf-8")) if dr_path and dr_path.exists() else None
    results = ROOT / "benchmarks" / "concurrent" / "results.jsonl"
    agg.concurrent = [json.loads(line) for line in results.read_text(encoding="utf-8").splitlines() if line.strip()] \
        if results.exists() else []
    outputs: dict[Path, str] = {CHART: render_svg(agg)}
    if agg.concurrent:
        images = ROOT / "docs" / "images"
        outputs[images / "bench-concurrent-throughput.svg"] = chart_lines(
            agg.concurrent, lambda row, wall: row["concurrency"] / wall,
            "Burst throughput: requests completed per second of wall time", "requests per second", True, 0.02)
        outputs[images / "bench-concurrent-rss.svg"] = chart_lines(
            agg.concurrent, lambda row, wall: row["peak_rss_kb"] / 1024 if row.get("peak_rss_kb") else None,
            "Peak resident memory of one process (MiB)", "MiB", True, None)
    for base in args.docs or [README]:
        for locale in LOCALES:
            target = doc_path(locale, base)
            if target.exists() and MARK.search(target.read_text(encoding="utf-8")):
                outputs[target] = render_doc(target, agg, locale)

    stale = []
    for target, content in outputs.items():
        current = target.read_text(encoding="utf-8") if target.exists() else None
        if current != content:
            stale.append(target)
            if not args.check:
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(content, encoding="utf-8")
    if args.check:
        if stale:
            for target in stale:
                print(f"render_bench_report: {target.relative_to(ROOT)} is not what {path.name} renders", file=sys.stderr)
            return 1
        print(f"render_bench_report: {len(outputs)} files match {path.name}")
        return 0
    print(f"render_bench_report: wrote {len(stale)} of {len(outputs)} files from {path.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
