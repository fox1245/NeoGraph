// Render the NeoGraph-vs-LangGraph bench charts to PNG.
//
// Server-side ECharts rendering via the `canvas` Node.js binding. No
// browser, no http server — just Canvas → PNG.

const echarts = require('echarts');
const { createCanvas } = require('canvas');
const fs = require('fs');

echarts.setPlatformAPI({
    createCanvas: (width, height) => createCanvas(width, height)
});

const NEO = '#2ea44f';
const LG  = '#8250df';

function renderLatency(canvas) {
    const chart = echarts.init(canvas);
    chart.setOption({
        animation: false,
        backgroundColor: '#ffffff',
        title: {
            text: 'Per-iteration latency (µs) — lower is better',
            subtext: 'NeoGraph is 31.2× faster on sequential, 14.8× faster on parallel fan-out',
            left: 'center',
            top: 16,
            textStyle: { fontSize: 18, fontWeight: 'bold', color: '#24292e' },
            subtextStyle: { fontSize: 13, color: '#586069' }
        },
        grid: { left: 88, right: 56, top: 100, bottom: 70 },
        legend: {
            bottom: 16,
            textStyle: { fontSize: 14, color: '#24292e' },
            itemGap: 32
        },
        xAxis: {
            type: 'category',
            data: ['3-node chain (seq)', 'Fan-out 5 + join (par)'],
            axisLabel: { fontSize: 14, color: '#24292e' },
            axisLine: { lineStyle: { color: '#d1d5da' } },
            axisTick: { lineStyle: { color: '#d1d5da' } }
        },
        yAxis: {
            type: 'value',
            name: 'µs / iteration',
            nameGap: 44,
            nameTextStyle: { fontSize: 13, color: '#586069' },
            axisLabel: { fontSize: 12, color: '#586069' },
            splitLine: { lineStyle: { color: '#eaecef' } }
        },
        series: [
            {
                name: 'NeoGraph',
                type: 'bar',
                data: [20.65, 150.70],
                itemStyle: { color: NEO, borderRadius: [4, 4, 0, 0] },
                label: {
                    show: true, position: 'top',
                    fontSize: 13, fontWeight: 'bold', color: NEO,
                    formatter: '{c} µs'
                },
                barWidth: 60
            },
            {
                name: 'LangGraph',
                type: 'bar',
                data: [645.30, 2225.12],
                itemStyle: { color: LG, borderRadius: [4, 4, 0, 0] },
                label: {
                    show: true, position: 'top',
                    fontSize: 13, fontWeight: 'bold', color: LG,
                    formatter: '{c} µs'
                },
                barWidth: 60
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
            subtext: 'Same workload, 12× less RAM',
            left: 'center',
            top: 16,
            textStyle: { fontSize: 18, fontWeight: 'bold', color: '#24292e' },
            subtextStyle: { fontSize: 13, color: '#586069' }
        },
        grid: { left: 72, right: 40, top: 100, bottom: 70 },
        legend: {
            bottom: 16,
            textStyle: { fontSize: 14, color: '#24292e' },
            itemGap: 32
        },
        xAxis: {
            type: 'category',
            data: ['Peak RSS'],
            axisLabel: { fontSize: 14, color: '#24292e' },
            axisLine: { lineStyle: { color: '#d1d5da' } },
            axisTick: { lineStyle: { color: '#d1d5da' } }
        },
        yAxis: {
            type: 'value',
            name: 'MB',
            nameGap: 36,
            nameTextStyle: { fontSize: 13, color: '#586069' },
            axisLabel: { fontSize: 12, color: '#586069' },
            splitLine: { lineStyle: { color: '#eaecef' } }
        },
        series: [
            {
                name: 'NeoGraph',
                type: 'bar',
                data: [4.9],
                itemStyle: { color: NEO, borderRadius: [4, 4, 0, 0] },
                label: {
                    show: true, position: 'top',
                    fontSize: 14, fontWeight: 'bold', color: NEO,
                    formatter: '{c} MB'
                },
                barWidth: 80
            },
            {
                name: 'LangGraph',
                type: 'bar',
                data: [58.9],
                itemStyle: { color: LG, borderRadius: [4, 4, 0, 0] },
                label: {
                    show: true, position: 'top',
                    fontSize: 14, fontWeight: 'bold', color: LG,
                    formatter: '{c} MB'
                },
                barWidth: 80
            }
        ]
    });
}

// Produce a single wide PNG with both charts side-by-side.
const w = 1400;
const h = 520;
const leftW = 880;
const rightW = w - leftW;

const fullCanvas = createCanvas(w, h);
const ctx = fullCanvas.getContext('2d');

// White canvas background
ctx.fillStyle = '#ffffff';
ctx.fillRect(0, 0, w, h);

// Render each chart on its own canvas then composite.
const leftCanvas = createCanvas(leftW, h);
renderLatency(leftCanvas);
ctx.drawImage(leftCanvas, 0, 0);

const rightCanvas = createCanvas(rightW, h);
renderRss(rightCanvas);
ctx.drawImage(rightCanvas, leftW, 0);

// Subtle divider line.
ctx.strokeStyle = '#e1e4e8';
ctx.lineWidth = 1;
ctx.beginPath();
ctx.moveTo(leftW, 40);
ctx.lineTo(leftW, h - 40);
ctx.stroke();

// Footer
ctx.fillStyle = '#6a737d';
ctx.font = '12px sans-serif';
ctx.textAlign = 'center';
ctx.fillText(
    '2026-04-18  ·  x86_64 Linux, g++ 13 (-O2), CPython 3.12.3, langgraph 1.1.7  ·  Reproduction: benchmarks/README.md',
    w / 2, h - 10
);

const out = fs.createWriteStream('/tmp/bench/bench-engine-overhead.png');
const stream = fullCanvas.createPNGStream();
stream.pipe(out);
out.on('finish', () => console.log('wrote bench-engine-overhead.png'));
