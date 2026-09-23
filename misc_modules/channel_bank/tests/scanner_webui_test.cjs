// Run with Node and Playwright available through NODE_PATH. No radio is opened.
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const {chromium} = require('playwright');
const source = fs.readFileSync(path.join(__dirname, '../src/main.cpp'), 'utf8');
const html = source.match(/R"HTML\(([\s\S]*?)\)HTML"/)[1];
const gain = value => ({available:true, running:false, devices:[], sampleRates:[],
  gains:[{name:'RF',value,min:0,max:50,step:1,available:true,liveMutable:true}]});
const state = {sources:['RX888','Airspy','RTL-SDR'], selectedSource:'RX888',
  running:false, radioPlaying:false, centerHz:132e6, sampleRate:2e6,
  sourceControls:gain(10), receiverControls:{Airspy:gain(20),'RTL-SDR':gain(30)},
  settings:{mode:'multi_receiver_scan',discoveryReceiver:'RX888',
    availableReceivers:[{id:'RX888',sampleRate:2e6},{id:'Airspy',sampleRate:2e6},{id:'RTL-SDR',sampleRate:2e6}],
    transmissionReceiverPool:['Airspy','RTL-SDR'],scanRanges:[{start:132e6,stop:133e6}],maximumMonitorSec:60},
  multiReceiverScan:{discoveryState:'STOPPED',receivers:[]}};
const posts = [];
(async () => {
  const browser = await chromium.launch({headless:true, executablePath:process.env.CHROME_PATH || undefined});
  try {
    const page = await browser.newPage();
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    await page.route('http://scanner.test/**', async route => {
      const req = route.request();
      if (req.method() === 'POST') {
        const body = req.postDataJSON();
        posts.push({path:new URL(req.url()).pathname,body});
        if (req.url().endsWith('/api/channel-bank/settings')) {
          Object.assign(state.settings, body);
          if (body.discoveryReceiver) state.selectedSource = body.discoveryReceiver;
        }
        // Simulate a slow proxy response to exercise latest-value-wins saves.
        await new Promise(resolve => setTimeout(resolve, 450));
      }
      await route.fulfill({contentType:req.url() === 'http://scanner.test/' ? 'text/html' : 'application/json',
        body:req.url() === 'http://scanner.test/' ? html : JSON.stringify(state)});
    });
    await page.goto('http://scanner.test/');
    await page.waitForFunction(() => document.getElementById('cbMode').value === 'multi_receiver_scan');
    assert.equal(await page.locator('#discoveryReceiver').inputValue(), 'RX888');
    assert.equal(await page.locator('#transcriptSection').isVisible(), false);
    const sectionOrder = await page.locator('section > h2, section > .span-head > h2').allTextContents();
    const spanIndex = sectionOrder.indexOf('Activity Span');
    assert.deepEqual(sectionOrder.slice(spanIndex + 1, spanIndex + 4),
      ['Active Channels', 'Playback / Transcript', 'Activity History']);
    state.history = [{freqHz:132e6, count:3, lastSeen:1, name:'Existing'}];
    state.activeChannels = [{freqHz:132e6, gridFreqHz:132e6, signalPresent:true, name:'Existing'},
      {freqHz:133e6, gridFreqHz:133e6, signalPresent:true, name:'New live'}];
    state.settings.transcriptionBackend = 2;
    state.lastTranscriptText = 'Test transcript';
    await page.waitForTimeout(800);
    assert.equal(await page.locator('#activityHistory tr').count(), 2);
    assert.equal(await page.locator('#activityHistory tr').filter({hasText:'Existing'}).locator('td').nth(2).textContent(), '3');
    assert.equal(await page.locator('#activityHistory tr').filter({hasText:'New live'}).locator('td').nth(3).textContent(), 'Live');
    assert.equal(await page.locator('#transcriptSection').isVisible(), true);
    state.activeChannels = [];
    state.history[0].count = 4;
    state.history[0].lastSeen = Date.now() / 1000;
    state.settings.transcriptionBackend = 0;
    await page.waitForTimeout(800);
    assert.equal(await page.locator('#activityHistory tr').count(), 1);
    assert.equal(await page.locator('#activityHistory tr td').nth(2).textContent(), '4');
    assert.equal(await page.locator('#transcriptSection').isVisible(), false);
    await page.locator('#sourceControlTarget').selectOption('Airspy');
    assert.equal(await page.locator('#srcGain_RF').inputValue(), '20');
    assert.equal(await page.locator('#srcSampleRate').isDisabled(), true);
    await page.locator('#srcGain_RF').fill('25');
    await page.waitForTimeout(400);
    await page.locator('#srcGain_RF').fill('31');
    await page.waitForTimeout(1300);
    const gains = posts.filter(p => p.path === '/api/source-controls');
    assert.equal(gains.at(-1).body.receiver, 'Airspy');
    assert.equal(gains.at(-1).body.gains.RF, 31);
    assert.equal(state.selectedSource, 'RX888');
    await page.locator('#discoveryReceiver').selectOption('RTL-SDR');
    await page.locator('[data-range="0"][data-edge="start"]').fill('134');
    await page.waitForTimeout(700);
    assert.equal(await page.locator('[data-range="0"][data-edge="start"]').inputValue(), '134');
    await page.locator('[data-range="0"][data-edge="stop"]').fill('135');
    await page.locator('#saveScanner').click();
    await page.waitForTimeout(800);
    const saved = posts.filter(p => p.path === '/api/channel-bank/settings').at(-1).body;
    assert.equal(saved.discoveryReceiver, 'RTL-SDR');
    assert.deepEqual(saved.transmissionReceiverPool, ['Airspy']);
    assert.deepEqual(saved.scanRanges, [{start:134e6,stop:135e6}]);
    state.running = true; state.radioPlaying = true;
    await page.waitForTimeout(800);
    assert.equal(await page.locator('#saveScanner').isDisabled(), true);
    assert.equal(await page.locator('#discoveryReceiver').isDisabled(), true);
    state.running = false; state.radioPlaying = false;
    await page.waitForTimeout(800);
    assert.equal(await page.locator('#saveScanner').isDisabled(), false);
    for (const width of [1440,390]) {
      await page.setViewportSize({width,height:1000});
      await page.locator('#scannerConfig').scrollIntoViewIfNeeded();
      for (const id of ['scannerConfig','sourceControlTarget']) {
        const box = await page.locator('#'+id).boundingBox();
        assert(box.x >= 0 && box.x + box.width <= width + 1, `${id} overflows at ${width}`);
      }
      if (process.env.SCREENSHOT_DIR) await page.screenshot({path:path.join(process.env.SCREENSHOT_DIR, `scanner-${width}.png`)});
    }
    assert.deepEqual(errors, []);
    console.log('Scanner WebUI routing, slow saves, drafts, mode, stop guards and viewport tests passed');
  } finally { await browser.close(); }
})().catch(error => {console.error(error);process.exitCode=1;});
