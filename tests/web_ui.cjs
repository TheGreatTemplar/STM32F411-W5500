const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const html = fs.readFileSync(path.join(__dirname, '../src/web/index.html'), 'utf8');
const source = html.match(/<script>([\s\S]*?)<\/script>/)[1].replace(/\n\s*update\(\);\s*$/, '');
const elements = new Map();
const requests = [];
let ledOn = false;
let failCommand = false;
let adcOk = true;
let failDiagnostics = false;
function createElement()
{
    return {textContent: '', className: '', disabled: true, open: false, children: [],
        append(...nodes)
        {
            this.children.push(...nodes);
        },
        replaceChildren()
        {
            this.children = [];
        },
        showModal()
        {
            this.open = true;
        },
        close()
        {
            this.open = false;
        },
        addEventListener(event, callback)
        {
            this[event] = callback;
        }};
}
const context = vm.createContext({
    document:
    {
        getElementById(id)
        {
            if (!elements.has(id))
            {
                elements.set(id, createElement());
            }
            return elements.get(id);
        },
        createElement
    },
    location: {host: '172.16.236.220:5025'},
    AbortController,
    setTimeout: () => 1,
    clearTimeout()
    {
    },
    async fetch(url, options)
    {
        requests.push({url, options});
        if (url === '/api/diagnostics')
        {
            return {ok: !failDiagnostics, status: failDiagnostics ? 503 : 200,
                json: async () => ({version: 4, phy: 7, mode: 0, interrupts: 128,
                    ip: '172.16.236.220', mask: '255.255.255.0', gateway: '172.16.236.1',
                    mac: '02:41:11:EC:00:DC', http_port: 5025, http_sockets: 1,
                    retry_ms: 200, retry_count: 3, init_attempts: 1, spi_errors: 0,
                    tx_timeouts: 0, rx_bytes: 200, tx_bytes: 2000, uptime_ms: 5000,
                    sockets: Array.from({length: 8}, (_, id) => ({id, state: id ? 0 : 23,
                        port: id ? 0 : 5025, peer: '172.16.236.100', peer_port: id ? 0 : 50000,
                        rx_kb: id ? 0 : 8, tx_kb: id ? 0 : 8, rx_waiting: 0, tx_free: id ? 0 : 8192, irq: 0}))})};
        }
        if (url.startsWith('/api/led/'))
        {
            assert.equal(options.method, 'POST');
            assert.equal(options.headers['X-Device-Control'], '1');
            if (failCommand)
            {
                return {ok: false, status: 500};
            }
            ledOn = url.endsWith('/on');
            return {ok: true, json: async () => ({led_on: ledOn})};
        }
        assert.equal(url, '/api/adc');
        return {ok: true, json: async () => ({raw: [0, 4095], mv: [0, 3300],
            adc_ok: adcOk, link: true, link_speed_mbps: 100, led_on: ledOn,
            sample_count: 1, uptime_ms: 1000, vref_mv: 3300})};
    }
});

async function test()
{
    vm.runInContext(source, context);
    await vm.runInContext('update()', context);
    assert.equal(elements.get('led-state').textContent, 'Выключен');
    assert.equal(elements.get('led-on').disabled, false);
    assert.equal(elements.get('address').textContent, '172.16.236.220:5025 · 100 Мбит/с');

    elements.get('led-on').click();
    assert.equal(elements.get('led-on').disabled, true);
    await vm.runInContext('update()', context);
    assert.equal(elements.get('led-state').textContent, 'Включён');
    assert.deepEqual(requests.slice(-2).map((r) => r.url), ['/api/led/on', '/api/adc']);
    assert.equal(elements.get('led-off').disabled, false);

    elements.get('led-off').click();
    await vm.runInContext('update()', context);
    assert.equal(ledOn, false);
    assert.equal(elements.get('led-state').textContent, 'Выключен');

    failCommand = true;
    elements.get('led-on').click();
    await vm.runInContext('update()', context);
    assert.equal(ledOn, false);
    assert.match(elements.get('led-message').textContent, /Нет подтверждения/);
    assert.equal(elements.get('led-on').disabled, true);
    failCommand = false;
    await vm.runInContext('update()', context);
    assert.equal(elements.get('led-on').disabled, false);

    adcOk = false;
    await vm.runInContext('update()', context);
    assert.equal(elements.get('led-state').textContent, 'Выключен');
    assert.equal(elements.get('led-on').disabled, false);
    elements.get('diag-open').click();
    assert.equal(elements.get('diagnostics').open, true);
    assert.equal(elements.get('diag-refresh').disabled, true);
    const requestsBefore = requests.length;
    await vm.runInContext('update()', context);
    assert.equal(requests.length, requestsBefore + 1);
    assert.equal(requests.at(-1).url, '/api/diagnostics');
    assert.equal(elements.get('diag-sockets').children.length, 8);
    assert.match(elements.get('diag-network').children[0].children[0].textContent, /0x04.*W5500/);
    assert.equal(elements.get('diag-refresh').disabled, false);
    assert.equal(elements.get('led-state').textContent, 'Выключен');
    failDiagnostics = true;
    elements.get('diag-refresh').click();
    await vm.runInContext('update()', context);
    assert.match(elements.get('diag-message').textContent, /Не удалось обновить/);
    assert.equal(elements.get('diag-sockets').children.length, 8);
    assert.equal(elements.get('led-state').textContent, 'Выключен');
    assert.equal(elements.get('diag-refresh').disabled, false);
    elements.get('diag-close').click();
    assert.equal(elements.get('diagnostics').open, false);
    console.log('Web UI: LED, serialized requests, diagnostic dialog, refresh and failure recovery passed');
}

test().catch((error) =>
{
    console.error(error);
    process.exitCode = 1;
});
