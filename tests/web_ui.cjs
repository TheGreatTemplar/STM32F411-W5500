const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const html = fs.readFileSync(path.join(__dirname, '../src/web/index.html'), 'utf8');
const source = html.match(/<script>([\s\S]*?)<\/script>/)[1].replace(/\n\s*update\(\);\s*$/, '');
const elements = new Map();
const requests = [];
const timers = new Map();
const browserStorage = new Map();
let timerId = 0;
let systemTimers = false;
let systemApplyCalls = 0;
let systemFail = false;
let uartFailTx = false;
let uartWire = '';
const uartState = {enabled:false,tx_busy:false,baud:115200,actual_baud:115107,bits:8,parity:0,stop:1,
    rx_total:0,tx_total:0,next:0,lost:0,more:0,errors:[0,0,0,0,0,0],rx:''};
const systemState = {ip: '172.16.236.220', mask: '255.255.255.0', gateway: '172.16.236.1', port: 5025,
    calibration: 0, sequence: 0, busy: false, error: 0, cal_pending: false, cal_applied: 0, cal_error: 0};
let holdAdc = null;
let holdSpi = null;
let confirmAllowed = true;
let pagedCatalog = false;
let ledOn = false;
let failCommand = false;
let adcOk = true;
let failDiagnostics = false;
let failMcu = false;
const mcuData = {firmware: {version: '0.1.0', build_utc: '2026-10-29T12:00:00Z',
    base_commit: '93dfd03301b8b5e29583e77c7495cc8bc5bafcf2', source_sha256: 'a'.repeat(64),
    web_crc32: '12345678', compiler: 'ArmClang 6190000', c_standard: 'C99 / CMSIS'},
    storage: {present: true, supported: true, complete: true, busy: false, capacity: 8388608,
        slot_bytes: 8192, bank_bytes: 524288, slots: 64, system_bytes: 8192,
        unallocated_bytes: 7331840, jedec: 'EF4017', record_version: 5, record_bytes: 4068,
        spi: {used: 2, free: 61, blocked: 1}, i2c: {used: 1, free: 63, blocked: 0}}};
let failSpi = false;
let failSpiTransport = false;
let failI2c = false;
let failConfig = false;
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
const saved = {present: true, ready: true, busy: false, scanning: false, scan: 64, id: 63, name: 'Default', has_saved: true, save_ok: true,
    error: 0, sequence: 7, settings: {mode: 3, hz: 25000000, lsb: 1, cs_high: 1, format: 'hex'},
    commands: ['9F 00 00 00', 'AA BB', '', '01 02']};
const libraryRecords = new Map([[63, structuredClone(saved)]]);
function createElement()
{
    const element = {textContent: '', className: '', disabled: true, open: false, children: [], attributes: {},
        closest()
        {
            return this.id === 'lang-en' || this.id === 'lang-ru' || this.id === 'lang-ja' || this.hasAttribute('data-user-text');
        },
        hasAttribute(name)
        {
            return Object.hasOwn(this.attributes, name);
        },
        getAttribute(name)
        {
            return this.attributes[name];
        },
        setAttribute(name, value)
        {
            this.attributes[name] = value;
        },
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
        focus()
        {
            this.focused = true;
        },
        close()
        {
            this.open = false;
        },
        addEventListener(event, callback)
        {
            this[event] = callback;
        }};
    Object.defineProperty(element, 'textContent',
    {
        get()
        {
            return this.firstChild ? this.firstChild.nodeValue : '';
        },
        set(value)
        {
            this.firstChild = {nodeValue: String(value), parentElement: this};
        }
    });
    return element;
}
const context = vm.createContext({
    Date: TestDate,
    TextEncoder,
    confirm: () => confirmAllowed,
    NodeFilter: {SHOW_TEXT: 4},
    localStorage:
    {
        getItem: (key) => browserStorage.get(key) || null,
        setItem: (key, value) => browserStorage.set(key, value)
    },
    document:
    {
        documentElement: {lang: 'ru'},
        body: {},
        createTreeWalker()
        {
            const nodes = [];
            const seen = new Set();
            function visit(element)
            {
                if (seen.has(element))
                {
                    return;
                }
                seen.add(element);
                if (element.firstChild)
                {
                    nodes.push(element.firstChild);
                }
                element.children.forEach(visit);
            }
            elements.forEach(visit);
            let index = 0;
            return {nextNode: () => nodes[index++] || null};
        },
        querySelectorAll(selector)
        {
            if (selector === '.spi-only' || selector === '.i2c-only')
            {
                return [];
            }
            return [...elements.values()].filter((element) => element.hasAttribute('aria-label') || element.hasAttribute('title'));
        },
        getElementById(id)
        {
            if (!elements.has(id))
            {
                elements.set(id, createElement());
                elements.get(id).id = id;
            }
            return elements.get(id);
        },
        createElement
    },
    location: {host: '172.16.236.220:5025'},
    AbortController,
    performance,
    setTimeout(callback, delay)
    {
        const id = ++timerId;
        timers.set(id, {callback, delay});
        if (systemTimers && (delay === 150 || delay === 500))
        {
            Promise.resolve().then(() =>
            {
                if (timers.delete(id))
                {
                    callback();
                }
            });
        }
        return id;
    },
    clearTimeout(id)
    {
        timers.delete(id);
    },
    async fetch(url, options)
    {
        requests.push({url, options});
        if (url.startsWith('/api/usart?from='))
        {
            const from = Number(url.split('=')[1]);
            return {ok:true,json:async()=>({...uartState,rx:uartWire.slice(from*2),next:uartWire.length/2})};
        }
        if (url === '/api/usart')
        {
            assert.equal(options.headers['X-Device-Control'],'1');
            if (options.body.startsWith('tx '))
            {
                if (uartFailTx)
                {
                    throw new Error('Lost TX acknowledgement');
                }
                const [header,...payload] = options.body.split('\n');
                const format = /format=(\d)/.exec(header)[1];
                const eol = Number(/eol=(\d)/.exec(header)[1]);
                const bytes = format === '0' ? payload.join('\n').replace(/\s/g,'') : Buffer.from(payload.join('\n'),'ascii').toString('hex');
                uartWire += (bytes + ['', '0D', '0A', '0D0A'][eol]).toUpperCase();
                uartState.rx_total = uartWire.length/2;
                uartState.tx_total = uartState.rx_total;
            }
            else
            {
                uartState.enabled = options.body.startsWith('start');
            }
            return {ok:true,json:async()=>({error:0})};
        }
        if (url.startsWith('/api/settings'))
        {
            if (options.method === 'POST')
            {
                if (systemFail)
                {
                    throw new Error('Lost settings reply');
                }
                const data = Object.fromEntries(options.body.trim().split('\n').map(line => line.split('=')));
                if (Number(data.seq) !== systemState.sequence)
                {
                    return {ok: false, json: async () => ({error: 12})};
                }
                if (url.endsWith('/apply'))
                {
                    ++systemApplyCalls;
                }
                else
                {
                    Object.assign(systemState, {ip: data.ip, mask: data.mask, gateway: data.gateway, port: Number(data.port),
                        calibration: Number(data.cal), cal_applied: Number(data.cal), sequence: systemState.sequence + 1});
                }
            }
            return {ok: true, json: async () => ({...systemState})};
        }
        if (url === '/api/library')
        {
            if (!options.method || options.body.startsWith('list '))
            {
                const offset = options.method ? Number(options.body.trim().split(' ')[1]) : 0;
                const sets = [...libraryRecords].map(([id, item]) => ({id, name: item.name, bus: item.settings.bus || 2}));
                return {ok: true, json: async () => ({scanning: false, used: sets.length, blocked: 0,
                    next: pagedCatalog && offset + 8 < 128 ? offset + 8 : null,
                    sets: pagedCatalog ? sets.filter((item) => item.id >= offset && item.id < offset + 8) : sets})};
            }
            if (options.body.startsWith('save '))
            {
                const bytes = new TextEncoder().encode(options.body);
                const first = bytes.indexOf(10);
                const parts = options.body.slice(0, first).split(' ');
                const name = new TextDecoder().decode(bytes.slice(first + 1, first + 1 + Number(parts[2])));
                let id = Number(parts[1]);
                const requestedBus = options.body.includes('\nbus=1 ') ? 1 : 2;
                if ([...libraryRecords].some(([key, item]) => key !== id && (item.settings.bus || 2) === requestedBus && item.name === name))
                {
                    return {ok: false, json: async () => ({error: 12})};
                }
                if (id === 128)
                {
                    id = options.body.includes('\nbus=1 ') ? 64 : 0;
                    while (libraryRecords.has(id))
                    {
                        ++id;
                    }
                }
                const snapshot = new TextDecoder().decode(bytes.slice(first + Number(parts[2]) + 2));
                const [header, lengths] = snapshot.split('\n');
                const fields = Object.fromEntries(header.split(' ').map(item => item.split('=')));
                const settings = fields.bus === '1' ? {bus: 1, hz: Number(fields.hz), address: Number(fields.addr), operation: Number(fields.op), read_size: Number(fields.read), format: fields.format} :
                    {bus: 2, mode: Number(fields.mode), hz: Number(fields.hz), lsb: Number(fields.lsb), cs_high: Number(fields.cs_high), format: fields.format};
                let payload = snapshot.slice(header.length + lengths.length + 2);
                const commands = lengths.slice(8).split(',').map(length =>
                {
                    const command = payload.slice(0, Number(length));
                    payload = payload.slice(Number(length));
                    return command;
                });
                const command_names = [];
                assert.ok(payload.startsWith('\nnames='));
                const endNames = payload.indexOf('\n', 1);
                let nameBytes = new TextEncoder().encode(payload.slice(endNames + 1));
                for (const size of payload.slice(7, endNames).split(',').map(Number))
                {
                    command_names.push(new TextDecoder().decode(nameBytes.slice(0, size)));
                    nameBytes = nameBytes.slice(size);
                }
                assert.equal(nameBytes.length, 0);
                Object.assign(saved, {id, name, has_saved: true, settings, commands, command_names});
                libraryRecords.set(id, structuredClone(saved));
                flashBusy = true;
                return {ok: true, json: async () => ({...saved, busy: true})};
            }
            if (options.body.startsWith('load '))
            {
                Object.assign(saved, structuredClone(libraryRecords.get(Number(options.body.trim().split(' ')[1]))));
            }
            if (options.body.startsWith('delete '))
            {
                const id = Number(options.body.trim().split(' ')[1]);
                libraryRecords.delete(id);
                if (saved.id === id)
                {
                    Object.assign(saved, {id: 128, name: '', has_saved: false});
                }
                flashBusy = true;
                return {ok: true, json: async () => ({...saved, busy: true})};
            }
            return {ok: true, json: async () => saved};
        }
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
        if (url === '/api/spi/config')
        {
            const fields = Object.fromEntries(options.body.trim().split(' ').map(item => item.split('=')));
            return {ok: !failConfig, json: async () => ({applied: true, mode: Number(fields.mode), hz: Number(fields.hz), lsb: Number(fields.lsb), cs_high: Number(fields.cs_high)})};
        }
        if (url === '/api/i2c')
        {
            assert.equal(options.method, 'POST');
            assert.equal(options.headers['X-Device-Control'], '1');
            const [header, text] = options.body.split('\n');
            const fields = Object.fromEntries(header.split(' ').map(item => item.split('=')));
            const tx = fields.format === 'ascii' ? Buffer.from(text, 'ascii').toString('hex').toUpperCase() : text.replace(/\s/g, '').toUpperCase();
            return {ok: !failI2c, status: failI2c ? 409 : 200, json: async () =>
                ({bus: 1, address: Number(fields.addr), operation: Number(fields.op), hz: 100000,
                    count: tx.length / 2, read_count: failI2c ? 0 : Number(fields.read), transfer_us: 123,
                    error: failI2c ? 1 : 0, tx, rx: failI2c ? '' : 'A0'.repeat(Number(fields.read))})};
        }
        if (url === '/api/spi')
        {
            assert.equal(options.method, 'POST');
            assert.equal(options.headers['X-Device-Control'], '1');
            if (holdSpi)
            {
                await holdSpi;
            }
            if (failSpiTransport)
            {
                throw new Error('Network failure');
            }
            return {ok: !failSpi, text: async () => 'Invalid SPI command',
                json: async () => ({bus: 2, mode: 3, hz: 1000000, count: 4,
                    tx: '9F000000', rx: 'EF401500', transfer_us: 42})};
        }
        if (url === '/api/mcu')
        {
            return {ok: !failMcu, json: async () => structuredClone(mcuData)};
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
        if (holdAdc)
        {
            await holdAdc;
        }
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
    assert.equal(elements.get('spi-hz').value, '25000000');
    assert.equal(elements.get('spi-mode-description').textContent, 'CPOL=1 · CPHA=1');
    assert.equal(elements.get('spi-lsb').checked, true);
    assert.equal(elements.get('spi-cs-high').checked, true);
    await vm.runInContext('update()', context); // Initial idle SPI configuration.
    const txBeforeConfig = requests.filter(item => item.url === '/api/spi').length;
    for (let i = 0; i < 4; ++i)
    {
        elements.get('spi-mode-' + i).checked = i === 2;
    }
    elements.get('spi-mode-2').change();
    for (let i = 0; i < 4; ++i)
    {
        elements.get('spi-mode-' + i).checked = i === 1;
    }
    elements.get('spi-mode-1').change();
    await vm.runInContext('update()', context);
    assert.equal(requests.at(-1).url, '/api/spi/config');
    assert.match(requests.at(-1).options.body, /mode=1.*format=hex\n$/);
    assert.equal(requests.filter(item => item.url === '/api/spi').length, txBeforeConfig);
    assert.equal(elements.get('spi-config-status').textContent, 'Настройки SPI применены');
    for (let i = 0; i < 4; ++i)
    {
        elements.get('spi-mode-' + i).checked = i === 3;
    }
    elements.get('spi-mode-3').change();
    failConfig = true;
    await vm.runInContext('update()', context);
    assert.equal(elements.get('spi-config-status').textContent, 'Нет подтверждения настроек SPI');
    const configsAfterFailure = requests.filter(item => item.url === '/api/spi/config').length;
    failConfig = false;
    await vm.runInContext('update()', context);
    assert.equal(requests.filter(item => item.url === '/api/spi/config').length, configsAfterFailure);
    elements.get('mcu-diag-open').click();
    await vm.runInContext('Promise.resolve()', context);
    assert.equal(elements.get('mcu-diagnostics').open, true);
    await vm.runInContext('loadMcu()', context);
    assert.equal(elements.get('storage-banks').children.length, 2);
    assert.match(elements.get('storage-banks').children[0].children[1].textContent, /2 \/ 64/);
    assert.match(elements.get('storage-banks').children[0].children[2].textContent, /^61/);
    assert.match(elements.get('storage-banks').children[0].children[3].textContent, /^1/);
    assert.equal(elements.get('about-info').children[0].children[0].textContent, '0.1.0');
    elements.get('about-open').click();
    assert.equal(elements.get('about-dialog').open, true);
    elements.get('about-close').click();
    assert.equal(elements.get('about-dialog').open, false);
    mcuData.storage.complete = false;
    await vm.runInContext('loadMcu()', context);
    assert.equal(elements.get('storage-banks').children[0].children[1].textContent, '—');
    mcuData.storage.complete = true;
    failMcu = true;
    await vm.runInContext('loadMcu()', context);
    assert.equal(elements.get('mcu-message').className, 'input-error');
    failMcu = false;
    elements.get('mcu-diag-close').click();
    assert.equal(elements.get('mcu-diagnostics').open, false);
    const beforeDialog = requests.length;
    elements.get('library-open-load').click();
    assert.equal(elements.get('command-library').open, true);
    assert.equal(elements.get('library-select').focused, true);
    assert.equal(elements.get('library-title').textContent, 'Загрузка набора');
    assert.equal(elements.get('library-name-row').hidden, true);
    assert.equal(elements.get('library-load').hidden, false);
    assert.equal(elements.get('store-save').hidden, true);
    assert.equal(elements.get('library-new').hidden, true);
    assert.equal(elements.get('library-delete').hidden, true);
    assert.equal(requests.length, beforeDialog);
    elements.get('library-close').click();
    assert.equal(elements.get('command-library').open, false);
    elements.get('library-open-save').click();
    assert.equal(elements.get('command-library').open, true);
    assert.equal(elements.get('library-name').focused, true);
    assert.equal(elements.get('library-title').textContent, 'Сохранение набора');
    assert.equal(elements.get('library-select-row').hidden, true);
    assert.equal(elements.get('library-name-row').hidden, false);
    assert.equal(elements.get('library-load').hidden, true);
    assert.equal(elements.get('store-save').hidden, false);
    assert.equal(elements.get('library-new').hidden, false);
    assert.equal(elements.get('library-delete').hidden, true);
    assert.equal(elements.get('library-message').textContent, elements.get('store-status').textContent);
    elements.get('library-close').click();
    await vm.runInContext('update()', context); // Initial library catalog.
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
    assert.equal(elements.get('spi-send-0').disabled, false);
    const beforeSpi = requests.length;
    await vm.runInContext('update()', context);
    assert.equal(requests.length, beforeSpi + 1);
    assert.equal(requests.at(-1).url, '/api/spi');
    assert.equal(requests.at(-1).options.body,
        'bus=2 mode=3 hz=1000000 lsb=0 cs_high=0 format=hex\n9F 00 00 00');
    assert.equal(elements.get('spi-tx-0').value, '9F 00 00 00');
    assert.equal(elements.get('spi-rx-0').value, 'EF 40 15 00');
    assert.equal(elements.get('spi-send-0').disabled, false);
    assert.match(elements.get('spi-message-0').textContent, /CS: 42 мкс.*отклик:/);
    elements.get('spi-command-0').value = '9';
    elements.get('spi-send-0').click();
    assert.match(elements.get('spi-message-0').textContent, /HEX/);
    assert.match(elements.get('spi-message-0').className, /input-error/);
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
    assert.equal(requests.at(-1).url, '/api/library');
    assert.match(requests.at(-1).options.body, /^save 63 7\nDefault\n/);
    assert.match(requests.at(-1).options.body, /lengths=5,5,0,5\n9F 00AA BB01 02\nnames=0,0,0,0\n$/);
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
    assert.equal(enter('9F GG'), '9F 0'); // Reject the whole mutation, never strip characters into a different command.
    assert.match(elements.get('spi-message-0').className, /input-error/);
    let prevented = false;
    field.beforeinput({data: 'G', preventDefault()
    {
        prevented = true;
    }});
    assert.equal(prevented, true);
    assert.equal(field.value, '9F 0');
    for (const text of ['0x9F', '9F GG', 'команда', '00 '.repeat(257)])
    {
        prevented = false;
        field.paste({clipboardData: {getData: () => text}, preventDefault()
        {
            prevented = true;
        }});
        assert.equal(prevented, true);
        assert.equal(field.value, '9F 0');
    }
    assert.equal(enter(''), '');
    assert.equal(enter('aa '), 'AA ');
    assert.equal(elements.get('spi-message-0').className, 'command-message');
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
    elements.get('spi-format').value = 'hex';
    enter('AA BB');
    failSpiTransport = false;
    const beforeWake = requests.filter((r) => r.url === '/api/spi').length;
    elements.get('spi-send-0').click();
    assert.equal([...timers.values()].at(-1).delay, 0);
    assert.equal(elements.get('spi-send-label-0').textContent, 'В очередь');
    await [...timers.values()].at(-1).callback();
    assert.equal(requests.filter((r) => r.url === '/api/spi').length, beforeWake + 1);
    assert.equal(elements.get('spi-send-0').disabled, false);

    let releaseAdc;
    holdAdc = new Promise((resolve) =>
    {
        releaseAdc = resolve;
    });
    const polling = vm.runInContext('update()', context);
    const beforeBusyWake = requests.length;
    elements.get('spi-send-0').click();
    await vm.runInContext('update()', context); // No parallel fetch while ADC is outstanding.
    assert.equal(requests.length, beforeBusyWake);
    holdAdc = null;
    releaseAdc();
    await polling;
    assert.equal([...timers.values()].at(-1).delay, 0);
    await [...timers.values()].at(-1).callback();
    assert.equal(requests.filter((r) => r.url === '/api/spi').length, beforeWake + 2);
    let releaseSpi;
    holdSpi = new Promise((resolve) =>
    {
        releaseSpi = resolve;
    });
    enter('AA 01');
    const beforeQueue = requests.filter((r) => r.url === '/api/spi').length;
    elements.get('spi-send-0').click();
    const transferring = vm.runInContext('update()', context);
    assert.equal(elements.get('spi-send-0').disabled, false);
    for (const byte of ['02', '03', '04', '05'])
    {
        enter('AA ' + byte);
        elements.get('spi-send-0').click();
    }
    assert.equal(elements.get('spi-send-0').disabled, true);
    assert.match(elements.get('spi-queue').textContent, /очередь: 4 \/ 4/);
    elements.get('spi-send-0').click(); // The queue guard also rejects programmatic clicks.
    assert.equal(vm.runInContext('pendingSpi.length', context), 4);
    assert.match(elements.get('spi-message-0').textContent, /Очередь заполнена/);
    const requestsBeforeQueuedLanguageChange = requests.length;
    elements.get('lang-en').click();
    assert.equal(elements.get('spi-send-label-0').textContent, 'Queue full');
    assert.equal(vm.runInContext('pendingSpi.length', context), 4);
    assert.equal(requests.length, requestsBeforeQueuedLanguageChange);
    elements.get('lang-ru').click();
    assert.equal(elements.get('spi-send-label-0').textContent, 'Очередь полна');
    holdSpi = null;
    releaseSpi();
    await transferring;
    for (let i = 0; i < 4; ++i)
    {
        assert.equal([...timers.values()].at(-1).delay, 0);
        await [...timers.values()].at(-1).callback();
    }
    const jobs = requests.filter((r) => r.url === '/api/spi').slice(beforeQueue);
    assert.equal(jobs.length, 5);
    assert.deepEqual(jobs.map((r) => r.options.body.split('\n')[1]), ['AA 01', 'AA 02', 'AA 03', 'AA 04', 'AA 05']);
    assert.equal(elements.get('spi-send-0').disabled, false);
    assert.match(elements.get('spi-queue').textContent, /очередь: 0 \/ 4/);
    const languageRequests = requests.length;
    const commandBeforeLanguage = field.value;
    elements.get('lang-en').click();
    assert.equal(context.document.documentElement.lang, 'en');
    assert.equal(context.document.title, 'STM32F411 Monitor');
    assert.equal(browserStorage.get('stm32f411-language'), 'en');
    assert.equal(elements.get('spi-send-label-0').textContent, 'Send');
    assert.match(elements.get('spi-message-0').textContent, /sent 4 bytes/);
    assert.equal(field.value, commandBeforeLanguage);
    assert.equal(requests.length, languageRequests);
    await vm.runInContext('update()', context);
    assert.equal(elements.get('led-state').textContent, 'Off');
    elements.get('lang-ru').click();
    assert.equal(elements.get('spi-send-label-0').textContent, 'Передать');
    assert.match(elements.get('spi-message-0').textContent, /передано 4 байт/);
    assert.equal(context.document.title, 'Монитор STM32F411');
    elements.get('lang-ja').click();
    assert.equal(context.document.documentElement.lang, 'ja');
    assert.equal(context.document.title, 'STM32F411 モニター');
    assert.equal(elements.get('spi-send-label-0').textContent, '送信');
    assert.match(elements.get('spi-message-0').textContent, /送信済み/);
    assert.equal(elements.get('library-select').children[1].textContent, 'SPI · Default');
    elements.get('lang-ru').click();
    context.localStorage = {setItem()
    {
        throw new Error('Storage blocked');
    }};
    elements.get('lang-en').click();
    assert.equal(elements.get('spi-send-label-0').textContent, 'Send');
    elements.get('lang-ru').click();
    pagedCatalog = true;
    elements.get('library-name').value = '日'.repeat(32);
    elements.get('library-name').input();
    elements.get('library-new').click();
    await vm.runInContext('update()', context);
    assert.match(requests.at(-1).options.body, /^save 128 96\n/);
    vm.runInContext('nextStorePoll = 0', context);
    await vm.runInContext('update()', context);
    for (let i = 0; i < 16; ++i)
    {
        await vm.runInContext('update()', context);
    }
    assert.equal(elements.get('library-select').children.length, 3);
    assert.equal(elements.get('library-select').value, '0');
    elements.get('library-new').click();
    await vm.runInContext('update()', context);
    assert.match(elements.get('store-status').textContent, /Такое имя уже существует/);
    assert.equal(elements.get('library-message').textContent, elements.get('store-status').textContent);
    assert.equal(elements.get('library-message').className, 'input-error');
    assert.equal(vm.runInContext('storeWaiting', context), false);
    const beforeLongName = requests.length;
    elements.get('library-name').value = '日'.repeat(33);
    elements.get('library-name').input();
    elements.get('library-new').click();
    assert.equal(requests.length, beforeLongName);
    elements.get('library-select').value = '63';
    elements.get('library-select').change();
    confirmAllowed = false;
    elements.get('library-load').click();
    assert.equal(vm.runInContext('pendingLibrary', context), null);
    confirmAllowed = true;
    const beforeLoadSpi = requests.filter((r) => r.url === '/api/spi').length;
    elements.get('library-load').click();
    await vm.runInContext('update()', context);
    assert.equal(elements.get('command-library').open, false);
    assert.equal(elements.get('library-name').value, 'Default');
    assert.equal(elements.get('spi-command-0').value, '9F 00');
    assert.equal(requests.filter((r) => r.url === '/api/spi').length, beforeLoadSpi);
    await vm.runInContext('update()', context); // Apply the loaded SPI settings without TX.
    elements.get('library-select').value = '0';
    const beforeDeleteDialog = requests.length;
    elements.get('library-open-delete').click();
    assert.equal(elements.get('command-library').className, 'delete-mode');
    assert.equal(elements.get('library-title').textContent, 'Удаление набора');
    assert.equal(elements.get('library-name-row').hidden, true);
    assert.equal(elements.get('library-select-row').hidden, false);
    assert.equal(elements.get('library-delete').hidden, false);
    assert.equal(elements.get('store-save').hidden, true);
    assert.equal(elements.get('library-load').hidden, true);
    assert.equal(elements.get('library-new').hidden, true);
    elements.get('library-delete').click();
    assert.equal(elements.get('library-delete-confirm').open, true);
    assert.equal(elements.get('library-delete-no').focused, true);
    assert.equal(requests.length, beforeDeleteDialog);
    assert.equal(vm.runInContext('pendingLibrary', context), null);
    const confirmedName = elements.get('library-delete-name').textContent;
    elements.get('lang-en').click();
    assert.equal(elements.get('library-delete-name').textContent, confirmedName);
    elements.get('lang-ru').click();
    elements.get('library-delete-no').click();
    assert.equal(elements.get('library-delete-confirm').open, false);
    assert.equal(vm.runInContext('deleteTarget', context), null);
    assert.equal(requests.length, beforeDeleteDialog);
    elements.get('library-delete').click();
    elements.get('library-delete-confirm').cancel(); // Escape is also a cancellation.
    elements.get('library-delete-confirm').close();
    assert.equal(vm.runInContext('deleteTarget', context), null);
    assert.equal(vm.runInContext('pendingLibrary', context), null);
    elements.get('library-delete').click();
    elements.get('library-select').value = '63'; // The confirmation pins the original ID.
    elements.get('library-delete-yes').click();
    elements.get('library-delete-yes').click(); // Double confirmation cannot queue another delete.
    assert.equal(vm.runInContext('pendingLibrary.id', context), 0);
    await vm.runInContext('update()', context);
    vm.runInContext('nextStorePoll = 0', context);
    await vm.runInContext('update()', context);
    for (let i = 0; i < 16; ++i)
    {
        await vm.runInContext('update()', context);
    }
    assert.equal(libraryRecords.has(0), false);
    assert.equal(elements.get('library-select').value, '63');
    const beforeBusSwitch = requests.length;
    const spiDraft = elements.get('spi-command-0').value;
    elements.get('bus-i2c').click();
    assert.equal(vm.runInContext('consoleBus', context), 1);
    assert.equal(requests.length, beforeBusSwitch);
    assert.equal(elements.get('spi-command-0').value, '');
    elements.get('spi-command-0').value = '00 10';
    elements.get('spi-command-0').input();
    elements.get('i2c-address').value = '02';
    elements.get('spi-send-0').click();
    assert.match(elements.get('spi-message-0').textContent, /08 до 77/);
    assert.equal(vm.runInContext('pendingSpi.length', context), 0);
    elements.get('i2c-address').value = '50';
    elements.get('i2c-operation').value = '2';
    elements.get('i2c-read').value = '2';
    elements.get('i2c-operation').change();
    elements.get('spi-send-0').click();
    assert.equal(elements.get('bus-spi').disabled, true);
    elements.get('bus-spi').click();
    assert.equal(vm.runInContext('consoleBus', context), 1);
    await vm.runInContext('update()', context);
    assert.equal(requests.at(-1).url, '/api/i2c');
    assert.equal(requests.at(-1).options.body, 'bus=1 hz=100000 addr=80 op=2 read=2 format=hex\n00 10');
    assert.equal(elements.get('spi-rx-0').value, 'A0 A0');
    assert.match(elements.get('spi-message-0').textContent, /I2C.*0x50.*TX: 2.*RX: 2/);
    elements.get('bus-spi').click();
    assert.equal(elements.get('spi-command-0').value, spiDraft);
    await vm.runInContext('update()', context);
    elements.get('bus-i2c').click();
    assert.equal(elements.get('spi-command-0').value, '00 10');
    assert.equal(elements.get('spi-rx-0').value, 'A0 A0');
    failI2c = true;
    elements.get('spi-send-0').click();
    await vm.runInContext('update()', context);
    assert.match(elements.get('spi-message-0').textContent, /NACK/);
    const afterNack = requests.filter(item => item.url === '/api/i2c').length;
    await vm.runInContext('update()', context);
    assert.equal(requests.filter(item => item.url === '/api/i2c').length, afterNack);
    failI2c = false;
    elements.get('i2c-operation').value = '1';
    elements.get('i2c-operation').change();
    assert.equal(elements.get('spi-command-0').disabled, true);
    elements.get('spi-send-0').click();
    await vm.runInContext('update()', context);
    assert.equal(requests.at(-1).options.body, 'bus=1 hz=100000 addr=80 op=1 read=2 format=hex\n');
    assert.equal(elements.get('spi-tx-0').value, '—');
    elements.get('library-name').value = 'I2C read';
    elements.get('library-name').input();
    elements.get('library-new').click();
    await vm.runInContext('update()', context);
    assert.match(requests.at(-1).options.body, /bus=1.*op=1 read=2.*\nlengths=0,0,0,0\n\nnames=0,0,0,0\n$/);
    vm.runInContext('nextStorePoll = 0', context);
    await vm.runInContext('update()', context);
    for (let i = 0; i < 16; ++i)
    {
        await vm.runInContext('update()', context);
    }
    elements.get('library-select').value = '63';
    elements.get('library-load').click();
    await vm.runInContext('update()', context);
    assert.equal(vm.runInContext('consoleBus', context), 2);
    assert.equal(elements.get('spi-command-0').disabled, false);
    await vm.runInContext('update()', context);
    elements.get('library-select').value = '64';
    elements.get('library-load').click();
    await vm.runInContext('update()', context);
    assert.equal(vm.runInContext('consoleBus', context), 1);
    assert.equal(elements.get('i2c-operation').value, '1');
    assert.equal(elements.get('i2c-read').value, '2');
    assert.equal(elements.get('library-name').value, 'I2C read');
    assert.equal(elements.get('library-select').children.length, 2);
    assert.equal(elements.get('library-select').children[1].textContent, 'I2C · I2C read');
    elements.get('lang-ja').click();
    assert.equal(elements.get('bus-title').textContent, 'I2C1 · 外部デバイス');
    for (const [i, name] of ['RFIC03', 'Проверка', '日本語', '😀'.repeat(32)].entries())
    {
        elements.get('spi-name-' + i).value = name;
        elements.get('spi-name-' + i).input();
    }
    const namedDraft = vm.runInContext('draftSnapshot()', context);
    elements.get('lang-en').click();
    assert.equal(elements.get('spi-name-1').value, 'Проверка');
    elements.get('spi-name-0').value = 'A'.repeat(49);
    elements.get('spi-name-0').input();
    assert.equal(elements.get('spi-name-0').value.length, 48);
    elements.get('spi-name-0').value = 'RFIC03';
    elements.get('spi-name-0').input();
    assert.equal(vm.runInContext('draftSnapshot()', context), namedDraft);
    elements.get('bus-spi').click();
    await vm.runInContext('update()', context);
    elements.get('bus-i2c').click();
    assert.equal(vm.runInContext('consoleBus', context), 1);
    assert.equal(elements.get('spi-name-2').value, '日本語');
    elements.get('library-name').value = 'I2C read';
    elements.get('store-save').click();
    await vm.runInContext('update()', context);
    for (let i = 0; i < 5 && !requests.at(-1).options.body?.startsWith('save '); ++i)
    {
        await vm.runInContext('update()', context);
    }
    assert.match(requests.at(-1).options.body, /names=6,16,9,128\nRFIC03Проверка日本語/);
    vm.runInContext('nextStorePoll = 0', context);
    await vm.runInContext('update()', context);
    assert.deepEqual(Array.from(saved.command_names), ['RFIC03', 'Проверка', '日本語', '😀'.repeat(32)]);
    elements.get('spi-name-1').value = 'Changed';
    elements.get('spi-name-1').input();
    vm.runInContext('showStore(' + JSON.stringify(saved) + ', true)', context);
    assert.equal(elements.get('spi-name-1').value, 'Changed');
    vm.runInContext('draftDirty = false; showStore(' + JSON.stringify(saved) + ', true)', context);
    assert.equal(elements.get('spi-name-1').value, 'Проверка');
    systemTimers = true;
    for (const id of ['calibration-start', 'calibration-end', 'calibration-hours'])
    {
        context.document.getElementById(id);
    }
    elements.get('lang-ru').click();
    assert.equal(elements.get('spi-name-left-3').textContent, '16');
    elements.get('spi-name-0').value = '😀'.repeat(49);
    elements.get('spi-name-0').input();
    assert.equal(Array.from(elements.get('spi-name-0').value).length, 48);
    assert.equal(elements.get('spi-name-left-0').textContent, '0');
    elements.get('i2c-operation').value = '0';
    elements.get('i2c-operation').change();
    elements.get('spi-command-0').value = 'AA BB CC';
    elements.get('spi-command-0').input();
    assert.equal(elements.get('spi-send-count-0').textContent, '3 Б');
    await vm.runInContext("openSystem('calibration')", context);
    elements.get('calibration-start').value = '0';
    elements.get('calibration-end').value = '3';
    elements.get('calibration-hours').value = '24';
    elements.get('calibration-calculate').click();
    assert.ok(Number(elements.get('calibration-ppm').value) < -34);
    assert.ok(Number(elements.get('calibration-ppm').value) > -35);
    await vm.runInContext('saveSystem(false)', context);
    assert.equal(systemState.calibration, -36);
    assert.equal(systemState.sequence, 1);
    assert.equal(systemApplyCalls, 0);
    const beforeReset = requests.filter(r => r.url === '/api/settings' && r.options.method === 'POST').length;
    elements.get('calibration-reset').click();
    assert.equal(elements.get('calibration-reset-confirm').open, true);
    assert.equal(systemState.calibration, -36);
    assert.equal(requests.filter(r => r.url === '/api/settings' && r.options.method === 'POST').length, beforeReset);
    elements.get('calibration-reset-no').click();
    assert.equal(elements.get('calibration-reset-confirm').open, false);
    assert.equal(systemState.calibration, -36);
    elements.get('calibration-reset').click();
    elements.get('calibration-reset-yes').click();
    await new Promise(setImmediate);
    assert.equal(elements.get('calibration-reset-confirm').open, false);
    assert.equal(requests.filter(r => r.url === '/api/settings' && r.options.method === 'POST').length, beforeReset + 1);
    assert.equal(systemState.calibration, 0);
    await vm.runInContext("openSystem('network')", context);
    elements.get('network-ip').value = '172.16.236.221';
    elements.get('network-port').value = '5026';
    await vm.runInContext('saveSystem(true)', context);
    assert.equal(systemState.port, 5026);
    assert.equal(systemApplyCalls, 1);
    assert.equal(elements.get('network-reconnect').href, 'http://172.16.236.221:5026/');
    const settingsWrites = requests.filter(r => r.url === '/api/settings' && r.options.method === 'POST').length;
    systemFail = true;
    await vm.runInContext('saveSystem(true)', context);
    assert.equal(requests.filter(r => r.url === '/api/settings' && r.options.method === 'POST').length, settingsWrites + 1);
    assert.equal(systemApplyCalls, 1);
    assert.equal(vm.runInContext('systemWriting', context), false);
    systemFail = false;
    ++systemState.sequence;
    await vm.runInContext('saveSystem(true)', context);
    assert.equal(systemState.sequence, 4);
    assert.equal(systemApplyCalls, 1);
    assert.match(elements.get('network-message').textContent, /другим клиентом/);
    elements.get('calibration-ppm').value = '1000';
    await vm.runInContext('saveSystem(false)', context);
    assert.equal(systemState.sequence, 4);
    assert.match(elements.get('calibration-message').textContent, /диапазона/);
    elements.get('network-port').value = '65536';
    await vm.runInContext('saveSystem(true)', context);
    assert.equal(systemState.port, 5026);
    assert.match(elements.get('network-message').textContent, /65535/);
    vm.runInContext("['uart-format','uart-eol','uart-command','uart-baud','uart-bits','uart-parity','uart-stop'].forEach(get)",context);
    elements.get('uart-format').value = '0';
    elements.get('uart-eol').value = '3';
    elements.get('uart-command').value = '55 AA';
    elements.get('bus-usart').click();
    await new Promise(setImmediate);
    assert.equal(elements.get('sync-console').hidden,true);
    assert.equal(elements.get('bus-library').hidden,true);
    assert.equal(elements.get('uart-console').hidden,false);
    elements.get('uart-baud').value = '115200';
    elements.get('uart-bits').value = '8';
    elements.get('uart-parity').value = '0';
    elements.get('uart-stop').value = '1';
    await vm.runInContext("controlUart('start')",context);
    await new Promise(setImmediate);
    assert.equal(uartState.enabled,true);
    await vm.runInContext("controlUart('tx')",context);
    await new Promise(setImmediate);
    assert.equal(uartWire,'55AA0D0A');
    assert.match(elements.get('uart-rx').value,/55 AA 0D 0A/);
    elements.get('uart-command').value = 'GZ';
    const uartWrites = requests.filter(r=>r.url==='/api/usart').length;
    await vm.runInContext("controlUart('tx')",context);
    assert.equal(requests.filter(r=>r.url==='/api/usart').length,uartWrites);
    elements.get('uart-command').value = '01';
    uartFailTx = true;
    await vm.runInContext("controlUart('tx')",context);
    assert.equal(requests.filter(r=>r.url==='/api/usart').length,uartWrites+1);
    assert.match(elements.get('uart-message').textContent,/Нет подтверждения/);
    uartFailTx = false;
    await vm.runInContext("controlUart('stop')",context);
    assert.equal(uartState.enabled,false);
    elements.get('bus-i2c').click();
    assert.equal(elements.get('uart-console').hidden,true);
    assert.equal(elements.get('sync-console').hidden,false);
    assert.equal(elements.get('bus-library').hidden,false);
    console.log('Web UI: SPI/I2C, USART DMA terminal/loopback/no retry, library, queue, RTC, Flash and About passed');
}

test().catch((error) =>
{
    console.error(error);
    process.exitCode = 1;
});
