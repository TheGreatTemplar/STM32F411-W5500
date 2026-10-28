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
let failSpi = false;
let failSpiTransport = false;
let flashBusy = false;
let failRtc = false;
let rtcState = {ready: true, set: true, lse_ready: true, error: 0, datetime: '2032-02-29T12:34:56'};
class TestDate extends Date
{
    getFullYear()
    {
        return 2026;
    }
    getMonth()
    {
        return 9;
    }
    getDate()
    {
        return 28;
    }
    getHours()
    {
        return 13;
    }
    getMinutes()
    {
        return 20;
    }
    getSeconds()
    {
        return 30;
    }
}
const saved = {present: true, ready: true, busy: false, has_saved: true, save_ok: true,
    error: 0, sequence: 7, settings: {mode: 3, hz: 20000000, lsb: 1, cs_high: 1, format: 'hex'},
    commands: ['9F 00 00 00', 'AA BB', '', '01 02']};
function createElement()
{
    return {textContent: '', className: '', disabled: true, open: false, children: [],
        selectionStart: 0, selectionEnd: 0,
        setSelectionRange(start, end)
        {
            this.selectionStart = start;
            this.selectionEnd = end;
        },
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
    Date: TestDate,
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
        if (url === '/api/rtc')
        {
            assert.equal(options.method, 'POST');
            assert.equal(options.headers['X-Device-Control'], '1');
            rtcState = {...rtcState, datetime: options.body, set: true};
            if (failRtc)
            {
                throw new Error('Lost RTC reply');
            }
            return {ok: true, json: async () => rtcState};
        }
        if (url === '/api/commands')
        {
            if (options.method === 'POST')
            {
                flashBusy = true;
                return {ok: true, json: async () => ({...saved, busy: true})};
            }
            flashBusy = false;
            return {ok: true, json: async () => ({...saved, busy: flashBusy})};
        }
        if (url === '/api/spi')
        {
            assert.equal(options.method, 'POST');
            assert.equal(options.headers['X-Device-Control'], '1');
            if (failSpiTransport)
            {
                throw new Error('Network failure');
            }
            return {ok: !failSpi, text: async () => 'Invalid SPI command',
                json: async () => ({bus: 2, mode: 3, hz: 1000000, count: 4,
                    tx: '9F000000', rx: 'EF401500'})};
        }
        if (url === '/api/diagnostics')
        {
            return {ok: !failDiagnostics, status: failDiagnostics ? 503 : 200,
                json: async () => ({version: 4, phy: 7, mode: 0, interrupts: 128,
                    ip: '172.16.236.220', mask: '255.255.255.0', gateway: '172.16.236.1',
                    mac: '02:41:11:EC:00:DC', http_port: 5025, http_sockets: 4,
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
            sample_count: 1, uptime_ms: 1000, vref_mv: 3300, rtc: rtcState})};
    }
});

async function test()
{
    vm.runInContext(source, context);
    await vm.runInContext('update()', context);
    assert.equal(elements.get('spi-command-1').value, 'AA BB');
    assert.equal(elements.get('spi-hz').value, '20000000');
    assert.equal(elements.get('spi-mode-description').textContent, 'CPOL=1 · CPHA=1');
    assert.equal(elements.get('spi-lsb').checked, true);
    assert.equal(elements.get('spi-cs-high').checked, true);
    await vm.runInContext('update()', context);
    assert.equal(elements.get('led-state').textContent, 'Выключен');
    assert.equal(elements.get('led-on').disabled, false);
    assert.equal(elements.get('address').textContent, '172.16.236.220:5025 · 100 Мбит/с');
    assert.equal(elements.get('rtc-value').textContent, '12:34:56');
    assert.equal(elements.get('rtc-date').textContent, '29.02.2032');
    assert.equal(elements.get('rtc-input-date').value, '2032-02-29');
    assert.equal(requests.filter((r) => r.url === '/api/rtc').length, 0);

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
    for (const id of ['spi-format', 'spi-command-0', 'spi-hz', 'spi-mode-0', 'spi-mode-1',
        'spi-mode-2', 'spi-mode-3', 'spi-lsb', 'spi-cs-high'])
    {
        context.document.getElementById(id);
    }
    elements.get('spi-format').value = 'hex';
    elements.get('spi-command-0').value = '9F 00 00 00';
    elements.get('spi-hz').value = '1000000';
    elements.get('spi-mode-3').checked = true;
    elements.get('spi-lsb').checked = false;
    elements.get('spi-cs-high').checked = false;
    elements.get('spi-send-0').click();
    assert.equal(elements.get('spi-send-0').disabled, true);
    const beforeSpi = requests.length;
    await vm.runInContext('update()', context);
    assert.equal(requests.length, beforeSpi + 1);
    assert.equal(requests.at(-1).url, '/api/spi');
    assert.equal(requests.at(-1).options.body,
        'bus=2 mode=3 hz=1000000 lsb=0 cs_high=0 format=hex\n9F 00 00 00');
    assert.equal(elements.get('spi-tx-0').value, '9F 00 00 00');
    assert.equal(elements.get('spi-rx-0').value, 'EF 40 15 00');
    assert.equal(elements.get('spi-send-0').disabled, false);
    elements.get('spi-command-0').value = '9';
    elements.get('spi-send-0').click();
    assert.match(elements.get('spi-message-0').textContent, /HEX/);
    elements.get('spi-command-0').value = '00 '.repeat(257);
    elements.get('spi-send-0').click();
    assert.match(elements.get('spi-message-0').textContent, /256/);
    elements.get('spi-format').value = 'ascii';
    elements.get('spi-command-0').value = 'АБВ';
    elements.get('spi-send-0').click();
    assert.match(elements.get('spi-message-0').textContent, /ASCII/);
    elements.get('spi-command-0').value = '*IDN?';
    failSpi = true;
    elements.get('spi-send-0').click();
    await vm.runInContext('update()', context);
    assert.match(elements.get('spi-message-0').textContent, /Invalid SPI/);
    failSpi = false;
    failSpiTransport = true;
    elements.get('spi-send-0').click();
    await vm.runInContext('update()', context);
    assert.match(elements.get('spi-message-0').textContent, /могла выполниться/);
    const afterFailure = requests.filter((r) => r.url === '/api/spi').length;
    await vm.runInContext('update()', context);
    assert.equal(requests.filter((r) => r.url === '/api/spi').length, afterFailure);
    failSpiTransport = false;
    elements.get('spi-format').value = 'hex';
    elements.get('spi-command-0').value = '9F 00';
    elements.get('spi-command-1').value = 'AA BB';
    elements.get('spi-send-1').click();
    await vm.runInContext('update()', context);
    assert.equal(elements.get('spi-rx-1').value, 'EF 40 15 00');
    assert.equal(elements.get('spi-command-0').value, '9F 00');
    const spiCount = requests.filter((r) => r.url === '/api/spi').length;
    elements.get('store-save').click();
    await vm.runInContext('update()', context);
    assert.equal(requests.at(-1).url, '/api/commands');
    assert.match(requests.at(-1).options.body, /lengths=5,5,0,5\n9F 00AA BB01 02$/);
    assert.equal(elements.get('store-save').disabled, true);
    vm.runInContext('nextStorePoll = 0', context);
    await vm.runInContext('update()', context);
    assert.equal(elements.get('store-save').disabled, false);
    assert.equal(requests.filter((r) => r.url === '/api/spi').length, spiCount);
    assert.equal(elements.get('spi-command-0').value, '9F 00');
    elements.get('spi-command-0').value = '55';
    elements.get('spi-command-0').input();
    vm.runInContext('showStore(' + JSON.stringify(saved) + ', true)', context);
    assert.equal(elements.get('spi-command-0').value, '55');
    const field = elements.get('spi-command-0');
    function enter(text, position = text.length)
    {
        field.value = text;
        field.setSelectionRange(position, position);
        field.input();
        return field.value;
    }
    assert.equal(enter('9f0000aa'), '9F 00 00 AA');
    assert.equal(field.selectionStart, 11);
    assert.equal(enter('aabb', 2), 'AA BB');
    assert.equal(field.selectionStart, 2); // Backspace can cross a byte separator.
    assert.equal(enter('9f\t00\nAa'), '9F 00 AA');
    assert.equal(enter('9f0'), '9F 0'); // Keep an unfinished nibble; never pad with zero.
    assert.equal(enter('9F GG'), '9F GG'); // Invalid data must remain visible for validation.
    assert.equal(enter(''), '');
    assert.equal(enter('aa '), 'AA ');
    assert.equal(enter('ff'.repeat(256)).length, 767);
    elements.get('spi-format').value = 'ascii';
    assert.equal(enter('aabb\n'), 'aabb\n');
    elements.get('rtc-input-date').value = '2025-02-29';
    elements.get('rtc-input-time').value = '10:00:00';
    elements.get('rtc-set').click();
    assert.match(elements.get('rtc-message').textContent, /существующую дату/);
    assert.equal(requests.filter((r) => r.url === '/api/rtc').length, 0);
    elements.get('rtc-input-date').value = '2000-01-01';
    elements.get('rtc-input-time').value = '00:00';
    elements.get('rtc-input-date').input();
    await vm.runInContext('update()', context);
    assert.equal(elements.get('rtc-input-date').value, '2000-01-01');
    elements.get('rtc-set').click();
    assert.equal(elements.get('rtc-set').disabled, true);
    await vm.runInContext('update()', context);
    assert.equal(requests.at(-1).options.body, '2000-01-01T00:00:00');
    assert.equal(elements.get('rtc-value').textContent, '00:00:00');
    elements.get('rtc-sync').click();
    await vm.runInContext('update()', context);
    assert.equal(requests.at(-1).options.body, '2026-10-28T13:20:30');
    assert.match(elements.get('rtc-message').textContent, /синхронизирован/);
    failRtc = true;
    elements.get('rtc-set').click();
    await vm.runInContext('update()', context);
    assert.match(elements.get('rtc-message').textContent, /Нет подтверждения/);
    const rtcCount = requests.filter((r) => r.url === '/api/rtc').length;
    await vm.runInContext('update()', context);
    assert.equal(requests.filter((r) => r.url === '/api/rtc').length, rtcCount);
    rtcState = {...rtcState, ready: false, set: false, error: 2};
    await vm.runInContext('update()', context);
    assert.equal(elements.get('rtc-value').textContent, '—');
    assert.equal(elements.get('rtc-set').disabled, true);
    assert.equal(elements.get('rtc-sync').disabled, true);
    assert.match(elements.get('rtc-message').textContent, /LSE не запустился/);
    console.log('Web UI: RTC manual/local-PC set, invalid date, no automatic sync/retry, LSE error, SPI/Flash/ADC/LED/diagnostics passed');
}

test().catch((error) =>
{
    console.error(error);
    process.exitCode = 1;
});
