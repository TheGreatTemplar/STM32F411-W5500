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
const context = vm.createContext({
    document:
    {
        getElementById(id)
        {
            if (!elements.has(id))
            {
                elements.set(id, {textContent: '', className: '', disabled: true,
                    addEventListener(event, callback)
                    {
                        this[event] = callback;
                    }});
            }
            return elements.get(id);
        }
    },
    location: {host: '172.16.236.220:5025'},
    AbortController,
    setTimeout: () => 1,
    clearTimeout: () => {},
    async fetch(url, options)
    {
        requests.push({url, options});
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
    console.log('Web UI: LED on/off, serialized requests, error recovery and ADC fault passed');
}

test().catch((error) =>
{
    console.error(error);
    process.exitCode = 1;
});
