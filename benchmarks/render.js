// Render the NeoGraph vs Python-frameworks bench chart to PNG.
//
// Server-side ECharts rendering via the `canvas` Node.js binding. No
// browser, no http server — just Canvas → PNG.

const echarts = require('echarts');
const { createCanvas } = require('canvas');
const fs = require('fs');

echarts.setPlatformAPI({
    createCanvas: (width, height) => createCanvas(width, height)
});

// One color per framework. NeoGraph green to stand out; others a cool palette.
const COLORS = {
    NeoGraph:        '#2ea44f',
    Haystack:        '#1f77b4',
    'pydantic-graph':'#17a2b8',
    LangGraph:       '#8250df',
    LlamaIndex:      '#d3822a',
    AutoGen:         '#c0392b',
};

const FRAMEWORKS = ['NeoGraph', 'Haystack', 'pydantic-graph', 'LangGraph', 'LlamaIndex', 'AutoGen'];

// NeoGraph numbers are 3.0 (feat/taskflow-removal, commit b95be11):
// the sync super-step loop now goes through run_sync(execute_graph_async),
// so seq ~46µs reflects one run_sync io_context per call. Other
// framework numbers are carried over from the 2.0-era measurement
// (2026-04-19) since those runtimes have not been re-baselined.
const SEQ_US = [46.10, 150.70, 240.34, 671.18, 1842.58, 3226.79];
const PAR_US = [114.40, 293.60, 308.32, 2396.30, 4781.24, 7389.42];

// Whole-process peak RSS (MB) — full script / binary run:
const RSS_MB = [5.5, 78.3, 34.9, 58.9, 101.5, 52.4];

function seriesFor(values, unit) {
    return FRAMEWORKS.map((fw, i) => ({
        name: fw,
        type: 'bar',
        data: FRAMEWORKS.map((_, j) => (j === i ? values[i] : null)),
        itemStyle: { color: COLORS[fw], borderRadius: [4, 4, 0, 0] },
        label: {
            show: true, position: 'top',
            fontSize: 11, fontWeight: 'bold', color: COLORS[fw],
            formatter: params => params.value != null ? `${params.value} ${unit}` : ''
        },
        barWidth: 42,
        barGap: '0%',
    }));
}

function renderLatency(canvas) {
    const chart = echarts.init(canvas);
    chart.setOption({
        animation: false,
        backgroundColor: '#ffffff',
        title: {
            text: 'Per-iteration engine overhead (µs, log scale) — lower is better',
            subtext: 'NeoGraph 3.0: 46.1 µs seq / 114.4 µs par.  Next-fastest Python (Haystack): 3.3× / 2.6× slower.',
            left: 'center',
            top: 24,
            itemGap: 12,
            textStyle: { fontSize: 17, fontWeight: 'bold', color: '#24292e' },
            subtextStyle: { fontSize: 12, color: '#586069' }
        },
        grid: { left: 80, right: 32, top: 150, bottom: 108, containLabel: true },
        legend: {
            bottom: 24,
            textStyle: { fontSize: 12, color: '#24292e' },
            itemGap: 24,
            data: ['seq (3-node chain)', 'par (fan-out 5 + join)'],
        },
        xAxis: [
            {
                type: 'category',
                data: FRAMEWORKS,
                axisLabel: { fontSize: 11, color: '#24292e', interval: 0 },
                axisLine: { lineStyle: { color: '#d1d5da' } },
                axisTick: { lineStyle: { color: '#d1d5da' } }
            }
        ],
        yAxis: {
            type: 'log',
            name: 'µs / iteration (log)',
            nameLocation: 'middle',
            nameRotate: 90,
            nameGap: 52,
            nameTextStyle: { fontSize: 12, color: '#586069' },
            axisLabel: { fontSize: 11, color: '#586069' },
            splitLine: { lineStyle: { color: '#eaecef' } },
            min: 10,
        },
        series: [
            {
                name: 'seq (3-node chain)',
                type: 'bar',
                data: SEQ_US,
                itemStyle: { color: '#2ea44f', borderRadius: [4, 4, 0, 0] },
                label: { show: true, position: 'top', fontSize: 11, color: '#24292e', formatter: '{c}' },
                barWidth: 26,
            },
            {
                name: 'par (fan-out 5 + join)',
                type: 'bar',
                data: PAR_US,
                itemStyle: { color: '#8250df', borderRadius: [4, 4, 0, 0] },
                label: { show: true, position: 'top', fontSize: 11, color: '#24292e', formatter: '{c}' },
                barWidth: 26,
            }
        ]
    });
}

function renderRss(canvas) {
    const chart = echarts.init(canvas);
    chart.setOption({
        animation: false,
        backgroundColor: '#ffffff',
        title: {
            text: 'Peak resident memory (MB) — lower is better',
            subtext: 'Full bench process (warm-up + seq + par). NeoGraph: 5.5 MB.  6–19× less than Python field.',
            left: 'center',
            top: 24,
            itemGap: 12,
            textStyle: { fontSize: 17, fontWeight: 'bold', color: '#24292e' },
            subtextStyle: { fontSize: 12, color: '#586069' }
        },
        grid: { left: 64, right: 32, top: 150, bottom: 108, containLabel: true },
        xAxis: {
            type: 'category',
            data: FRAMEWORKS,
            axisLabel: { fontSize: 11, color: '#24292e', interval: 0 },
            axisLine: { lineStyle: { color: '#d1d5da' } },
            axisTick: { lineStyle: { color: '#d1d5da' } }
        },
        yAxis: {
            type: 'value',
            name: 'MB',
            nameLocation: 'middle',
            nameRotate: 90,
            nameGap: 44,
            nameTextStyle: { fontSize: 12, color: '#586069' },
            axisLabel: { fontSize: 11, color: '#586069' },
            splitLine: { lineStyle: { color: '#eaecef' } }
        },
        series: [
            {
                name: 'Peak RSS',
                type: 'bar',
                data: FRAMEWORKS.map((fw, i) => ({
                    value: RSS_MB[i],
                    itemStyle: { color: COLORS[fw], borderRadius: [4, 4, 0, 0] }
                })),
                label: {
                    show: true, position: 'top',
                    fontSize: 12, fontWeight: 'bold',
                    color: '#24292e',
                    formatter: '{c} MB'
                },
                barWidth: 52,
            }
        ]
    });
}

const w = 1700;
const h = 640;
const leftW = 1020;
const rightW = w - leftW;

const fullCanvas = createCanvas(w, h);
const ctx = fullCanvas.getContext('2d');

ctx.fillStyle = '#ffffff';
ctx.fillRect(0, 0, w, h);

const leftCanvas = createCanvas(leftW, h);
renderLatency(leftCanvas);
ctx.drawImage(leftCanvas, 0, 0);

const rightCanvas = createCanvas(rightW, h);
renderRss(rightCanvas);
ctx.drawImage(rightCanvas, leftW, 0);

ctx.strokeStyle = '#e1e4e8';
ctx.lineWidth = 1;
ctx.beginPath();
ctx.moveTo(leftW, 40);
ctx.lineTo(leftW, h - 40);
ctx.stroke();

ctx.fillStyle = '#6a737d';
ctx.font = '11px sans-serif';
ctx.textAlign = 'center';
ctx.fillText(
    'NeoGraph 2026-04-22 (3.0), Python field 2026-04-19  ·  x86_64 Linux, g++ 13 (-O2), CPython 3.12.3  ·  langgraph 1.1.9, haystack-ai 2.27, pydantic-graph 1.84, llama-index 0.14, autogen-agentchat 0.7.5  ·  Reproduction: benchmarks/README.md',
    w / 2, h - 10
);

const outPath = '../docs/images/bench-engine-overhead.png';
const out = fs.createWriteStream(outPath);
const stream = fullCanvas.createPNGStream();
stream.pipe(out);
out.on('finish', () => console.log('wrote', outPath));
