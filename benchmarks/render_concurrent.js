// Render concurrent-bench PNGs.
//
//   docs/images/bench-concurrent-throughput.png
//   docs/images/bench-concurrent-latency.png
//   docs/images/bench-concurrent-rss.png
//
// Feeds from the 1 CPU / 512 MB results in
// benchmarks/concurrent/results.jsonl — the tighter profile, where
// the scaling story is sharpest. The 2 CPU / 1 GB numbers tell the
// same story with a gentler slope.

const echarts = require('echarts');
const { createCanvas } = require('canvas');
const fs = require('fs');
const path = require('path');

echarts.setPlatformAPI({ createCanvas: (w, h) => createCanvas(w, h) });

const NEO = '#2ea44f';
const LG_ASYNC = '#8250df';
const LG_MP    = '#cf222e';  // red — the "production Python" config

// ── Parse results.jsonl ───────────────────────────────────────────────

const RESULTS_PATH = process.env.RESULTS_PATH
    || '/root/Coding/NeoGraph/benchmarks/concurrent/results.jsonl';
const PROFILE = '1:512m';

const rows = fs.readFileSync(RESULTS_PATH, 'utf8')
    .split('\n').filter(Boolean).map(JSON.parse);

function pick(engineMode) {
    const out = rows
        .filter(r => r.profile === PROFILE)
        .filter(r => {
            if (engineMode === 'neograph') return r.engine === 'neograph';
            if (engineMode === 'lg-async') return r.engine === 'langgraph' && r.mode === 'asyncio';
            if (engineMode === 'lg-mp') return r.engine === 'langgraph' && r.mode && r.mode.startsWith('mp');
            return false;
        })
        .sort((a, b) => a.concurrency - b.concurrency);
    return out;
}

const ng = pick('neograph');
const la = pick('lg-async');
const lm = pick('lg-mp');

const concurrencies = [...new Set(rows.map(r => r.concurrency))].sort((a, b) => a - b);

function throughput(row) {
    // req/sec; clamp wall to 1ms so log scale handles the sub-ms NG case.
    const wallSec = Math.max(row.total_wall_ms, 1) / 1000.0;
    return row.concurrency / wallSec;
}

// ── Shared chart options ─────────────────────────────────────────────

const CANVAS = { width: 1400, height: 620 };
const FOOTER_TEXT =
    'Profile: 1 CPU / 512 MB (Docker --cpus=1 --memory=512m)  ·  Workload: 3-node seq counter chain  ·  See benchmarks/concurrent/ for reproduction';

function withFooter(canvas, footerText) {
    const ctx = canvas.getContext('2d');
    ctx.fillStyle = '#f6f8fa';
    ctx.fillRect(0, canvas.height - 32, canvas.width, 32);
    ctx.strokeStyle = '#e1e4e8';
    ctx.beginPath();
    ctx.moveTo(0, canvas.height - 32);
    ctx.lineTo(canvas.width, canvas.height - 32);
    ctx.stroke();
    ctx.fillStyle = '#6a737d';
    ctx.font = '12px sans-serif';
    ctx.textAlign = 'center';
    ctx.fillText(footerText, canvas.width / 2, canvas.height - 12);
}

function renderLine(optsBase, outPath) {
    const canvas = createCanvas(CANVAS.width, CANVAS.height);
    const chart = echarts.init(canvas);
    chart.setOption(Object.assign({}, optsBase, { animation: false, backgroundColor: '#ffffff' }));
    withFooter(canvas, FOOTER_TEXT);
    const stream = canvas.createPNGStream();
    stream.pipe(fs.createWriteStream(outPath));
}

// ── Chart 1: Throughput (req/sec, log Y) ─────────────────────────────

renderLine({
    title: {
        text: 'Throughput under concurrent load — requests per second (log scale, higher is better)',
        subtext: 'At 10,000 simultaneous requests NeoGraph processes ~1340× the LangGraph-mp rate and ~3400× the asyncio rate',
        left: 24, top: 16,
        textStyle: { fontSize: 18, fontWeight: 'bold', color: '#24292e' },
        subtextStyle: { fontSize: 13, color: '#586069' }
    },
    grid: { left: 100, right: 48, top: 100, bottom: 130 },
    legend: {
        bottom: 50,
        textStyle: { fontSize: 13, color: '#24292e' },
        itemGap: 40
    },
    tooltip: { trigger: 'axis' },
    xAxis: {
        type: 'log',
        axisLabel: {
            fontSize: 12,
            formatter: v => v.toLocaleString()
        },
        splitLine: { lineStyle: { color: '#eaecef' } },
        minorSplitLine: { show: false }
    },
    yAxis: {
        type: 'log',
        axisLabel: {
            fontSize: 11,
            formatter: v => v.toLocaleString() + ' req/s'
        },
        splitLine: { lineStyle: { color: '#eaecef' } }
    },
    series: [
        {
            name: 'NeoGraph (std::thread pool)',
            type: 'line',
            symbol: 'circle', symbolSize: 9,
            data: ng.map(r => [r.concurrency, throughput(r)]),
            itemStyle: { color: NEO },
            lineStyle: { color: NEO, width: 3 },
            label: {
                show: true,
                position: 'top',
                formatter: p => Math.round(p.value[1]).toLocaleString(),
                fontSize: 11,
                color: NEO,
                fontWeight: 'bold'
            }
        },
        {
            name: 'LangGraph mp-pool (7 procs)',
            type: 'line',
            symbol: 'circle', symbolSize: 9,
            data: lm.map(r => [r.concurrency, throughput(r)]),
            itemStyle: { color: LG_MP },
            lineStyle: { color: LG_MP, width: 3 },
            label: {
                show: true,
                position: 'bottom',
                formatter: p => Math.round(p.value[1]).toLocaleString(),
                fontSize: 11,
                color: LG_MP,
                fontWeight: 'bold'
            }
        },
        {
            name: 'LangGraph asyncio (1 proc)',
            type: 'line',
            symbol: 'circle', symbolSize: 9,
            data: la.map(r => [r.concurrency, throughput(r)]),
            itemStyle: { color: LG_ASYNC },
            lineStyle: { color: LG_ASYNC, width: 3 },
            label: {
                show: true,
                position: 'bottom',
                formatter: p => Math.round(p.value[1]).toLocaleString(),
                fontSize: 11,
                color: LG_ASYNC,
                fontWeight: 'bold'
            }
        }
    ]
}, '/tmp/bench/bench-concurrent-throughput.png');

// ── Chart 2: P99 latency (µs, log Y) ─────────────────────────────────

renderLine({
    title: {
        text: 'Tail latency — P99 per request (log scale, lower is better)',
        subtext: 'NeoGraph stays in microseconds as load scales; asyncio\'s P99 grows linearly because GIL serializes the 10k coroutines',
        left: 24, top: 16,
        textStyle: { fontSize: 18, fontWeight: 'bold', color: '#24292e' },
        subtextStyle: { fontSize: 13, color: '#586069' }
    },
    grid: { left: 100, right: 48, top: 100, bottom: 130 },
    legend: {
        bottom: 50,
        textStyle: { fontSize: 13, color: '#24292e' },
        itemGap: 40
    },
    tooltip: { trigger: 'axis' },
    xAxis: {
        type: 'log',
        axisLabel: { fontSize: 12, formatter: v => v.toLocaleString() },
        splitLine: { lineStyle: { color: '#eaecef' } }
    },
    yAxis: {
        type: 'log',
        axisLabel: {
            fontSize: 11,
            formatter: v => {
                if (v >= 1_000_000) return (v / 1_000_000) + ' s';
                if (v >= 1_000) return (v / 1_000) + ' ms';
                return v + ' µs';
            }
        },
        splitLine: { lineStyle: { color: '#eaecef' } }
    },
    series: [
        {
            name: 'NeoGraph',
            type: 'line',
            symbol: 'circle', symbolSize: 9,
            data: ng.map(r => [r.concurrency, Math.max(r.p99_us, 1)]),
            itemStyle: { color: NEO },
            lineStyle: { color: NEO, width: 3 },
            label: { show: true, position: 'top', fontSize: 11, color: NEO, fontWeight: 'bold',
                     formatter: p => {
                         const v = p.value[1];
                         return v >= 1000 ? (v / 1000).toFixed(1) + ' ms' : v + ' µs';
                     } }
        },
        {
            name: 'LangGraph mp-pool',
            type: 'line',
            symbol: 'circle', symbolSize: 9,
            data: lm.map(r => [r.concurrency, Math.max(r.p99_us, 1)]),
            itemStyle: { color: LG_MP },
            lineStyle: { color: LG_MP, width: 3 },
            label: { show: true, position: 'bottom', fontSize: 11, color: LG_MP, fontWeight: 'bold',
                     formatter: p => (p.value[1] / 1000).toFixed(0) + ' ms' }
        },
        {
            name: 'LangGraph asyncio',
            type: 'line',
            symbol: 'circle', symbolSize: 9,
            data: la.map(r => [r.concurrency, Math.max(r.p99_us, 1)]),
            itemStyle: { color: LG_ASYNC },
            lineStyle: { color: LG_ASYNC, width: 3 },
            label: { show: true, position: 'top', fontSize: 11, color: LG_ASYNC, fontWeight: 'bold',
                     formatter: p => {
                         const v = p.value[1];
                         if (v >= 1_000_000) return (v / 1_000_000).toFixed(1) + ' s';
                         if (v >= 1000) return (v / 1000).toFixed(0) + ' ms';
                         return v + ' µs';
                     } }
        }
    ]
}, '/tmp/bench/bench-concurrent-latency.png');

// ── Chart 3: Peak RSS (MB, linear Y) ─────────────────────────────────

renderLine({
    title: {
        text: 'Peak resident memory under concurrent load (MB, lower is better)',
        subtext: 'NeoGraph stays near 5–8 MB at 10k concurrent; asyncio grows linearly to 426 MB because every coroutine holds a stack frame',
        left: 24, top: 16,
        textStyle: { fontSize: 18, fontWeight: 'bold', color: '#24292e' },
        subtextStyle: { fontSize: 13, color: '#586069' }
    },
    grid: { left: 100, right: 48, top: 100, bottom: 130 },
    legend: {
        bottom: 50,
        textStyle: { fontSize: 13, color: '#24292e' },
        itemGap: 40
    },
    tooltip: { trigger: 'axis' },
    xAxis: {
        type: 'log',
        axisLabel: { fontSize: 12, formatter: v => v.toLocaleString() },
        splitLine: { lineStyle: { color: '#eaecef' } }
    },
    yAxis: {
        type: 'value',
        axisLabel: { fontSize: 11, formatter: '{value} MB' },
        splitLine: { lineStyle: { color: '#eaecef' } }
    },
    series: [
        {
            name: 'NeoGraph',
            type: 'line',
            symbol: 'circle', symbolSize: 9,
            data: ng.map(r => [r.concurrency, r.peak_rss_kb / 1024]),
            itemStyle: { color: NEO },
            lineStyle: { color: NEO, width: 3 },
            label: { show: true, position: 'bottom', fontSize: 11, color: NEO, fontWeight: 'bold',
                     formatter: p => p.value[1].toFixed(1) + ' MB' }
        },
        {
            name: 'LangGraph mp-pool',
            type: 'line',
            symbol: 'circle', symbolSize: 9,
            data: lm.map(r => [r.concurrency, r.peak_rss_kb / 1024]),
            itemStyle: { color: LG_MP },
            lineStyle: { color: LG_MP, width: 3 },
            label: { show: true, position: 'bottom', fontSize: 11, color: LG_MP, fontWeight: 'bold',
                     formatter: p => p.value[1].toFixed(0) + ' MB' }
        },
        {
            name: 'LangGraph asyncio',
            type: 'line',
            symbol: 'circle', symbolSize: 9,
            data: la.map(r => [r.concurrency, r.peak_rss_kb / 1024]),
            itemStyle: { color: LG_ASYNC },
            lineStyle: { color: LG_ASYNC, width: 3 },
            label: { show: true, position: 'top', fontSize: 11, color: LG_ASYNC, fontWeight: 'bold',
                     formatter: p => p.value[1].toFixed(0) + ' MB' }
        }
    ]
}, '/tmp/bench/bench-concurrent-rss.png');

console.log('rendered 3 concurrent bench charts');
