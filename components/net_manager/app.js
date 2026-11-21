(() => {
  'use strict';

  const MODE_NAMES = ['離線模式 (OFFLINE)', '在線模式 (ONLINE)', '網頁設定 (WEB_CFG)'];
  const WIFI_STATES = ['未連線 / 關閉', '連線中...', '已連線 (STA)', 'AP 熱點模式'];
  const POLL_INTERVAL_MS = 2500;
  const POLL_TIMEOUT_MS = 1500;
  const CTRL_TIMEOUT_MS = 2500;

  let reqGen = 0;
  let pollTimer = null;
  let pollAbort = null;
  let toastTimer = null;
  let isCtrlLocked = false;
  let lastSeenErrSeq = null;

  const $ = (id) => document.getElementById(id);

  function showToast(text, type = 'err') {
    const el = $('toast');
    if (!el || !text) return;
    el.textContent = text;
    el.className = 'toast show ' + type;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => {
      el.classList.remove('show');
    }, 3000);
  }

  function showFormMsg(ok, text) {
    const el = $('msg');
    if (!el) return;
    el.className = 'msg ' + (ok ? 'ok' : 'err');
    el.textContent = text;
  }

  function setControlsLocked(locked) {
    isCtrlLocked = locked;
    document.querySelectorAll('.ctrl-btn').forEach((btn) => {
      btn.disabled = locked;
    });
    $('sw_wifi').disabled = locked;
    $('sw_cloud').disabled = locked;
  }

  function applyState(s) {
    if (!s || typeof s !== 'object') return;

    $('st_mode').textContent = MODE_NAMES[s.mode] || ('模式 ' + s.mode);
    $('st_wifi').textContent = WIFI_STATES[s.wifi_state] || ('狀態 ' + s.wifi_state);
    $('st_ssid').textContent = s.wifi_ssid || '(未設定)';
    $('st_eeprom').textContent = (s.eeprom_records ?? 0) + ' 筆';
    $('st_sd').textContent = s.sd_mounted ? ('已掛載 / 剩餘 ' + s.sd_free_mb + ' MB') : '未掛載';

    $('st_temp').textContent = s.sensor_valid ? (Number(s.temp_c).toFixed(1) + ' °C') : '--';
    $('st_humi').textContent = s.sensor_valid ? (Number(s.humi_pct).toFixed(1) + ' %') : '--';
    $('st_lux').textContent = s.sensor_valid ? (s.lux + ' lx') : '--';

    $('sw_wifi').checked = Boolean(s.wifi_en);
    $('sw_cloud').checked = Boolean(s.cloud_en);

    const vol = Number(s.volume_pct ?? 0);
    $('st_vol').textContent = s.is_muted ? ('靜音 (' + vol + '%)') : ('音量 ' + vol + '%');

    const btnMute = $('btn_mute');
    btnMute.classList.toggle('active', Boolean(s.is_muted));
    btnMute.lastChild.textContent = s.is_muted ? ' 取消靜音' : ' 靜音';

    const btnMusic = $('btn_music');
    btnMusic.classList.toggle('active', Boolean(s.is_music_playing));
    btnMusic.lastChild.textContent = s.is_music_playing ? ' 停止音樂' : ' 播放音樂';

    if (typeof s.err_seq === 'number') {
      if (lastSeenErrSeq === null) {
        lastSeenErrSeq = s.err_seq;
      } else if (s.err_seq > lastSeenErrSeq) {
        lastSeenErrSeq = s.err_seq;
        if (s.last_err) showToast(s.last_err, 'err');
      }
    }
  }

  async function fetchWithTimeout(url, options, timeoutMs) {
    const ctrl = new AbortController();
    const timer = setTimeout(() => ctrl.abort(), timeoutMs);
    try {
      return await fetch(url, { ...options, signal: ctrl.signal });
    } finally {
      clearTimeout(timer);
    }
  }

  async function loadStatus() {
    if (document.hidden || isCtrlLocked) return;
    const myGen = reqGen;
    if (pollAbort) pollAbort.abort();
    const ctrl = new AbortController();
    pollAbort = ctrl;
    const timer = setTimeout(() => ctrl.abort(), POLL_TIMEOUT_MS);

    try {
      const r = await fetch('/status', { cache: 'no-store', signal: ctrl.signal });
      if (!r.ok || myGen !== reqGen) return;
      const data = await r.json();
      if (myGen === reqGen) applyState(data);
    } catch (_) {
      // 逾時或主動中斷時靜默略過，等待下次輪詢
    } finally {
      clearTimeout(timer);
      if (pollAbort === ctrl) pollAbort = null;
    }
  }

  async function sendControl(cmd, val = 1) {
    if (isCtrlLocked) return;
    reqGen += 1;
    if (pollAbort) {
      pollAbort.abort();
      pollAbort = null;
    }
    setControlsLocked(true);

    try {
      const r = await fetchWithTimeout('/api/control', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ cmd, val })
      }, CTRL_TIMEOUT_MS);
      const res = await r.json();
      if (res.state) applyState(res.state);
      if (!res.ok) {
        showToast(res.msg || '操作失敗', 'err');
      }
    } catch (_) {
      showToast('指令逾時或連線中斷', 'err');
    } finally {
      setControlsLocked(false);
    }
  }

  async function loadConfig() {
    try {
      const r = await fetchWithTimeout('/config', { cache: 'no-store' }, 2000);
      if (!r.ok) return;
      const d = await r.json();
      $('wifi_ssid').value = d.wifi_ssid || '';
      $('wifi_pass').placeholder = d.has_pass ? '已設定密碼（留空則不修改）' : '開放網路或輸入 8~63 字元密碼';
      $('server_url').value = d.server_url || '';
      $('sample_interval_sec').value = d.sample_interval_sec;
      $('psram_window_hours').value = d.psram_window_hours;
      $('eeprom_warn_pct').value = d.eeprom_warn_pct;
      $('sd_warn_free_mb').value = d.sd_warn_free_mb;
    } catch (_) {}
  }

  function startPolling() {
    stopPolling();
    pollTimer = setInterval(loadStatus, POLL_INTERVAL_MS);
  }

  function stopPolling() {
    if (pollTimer) {
      clearInterval(pollTimer);
      pollTimer = null;
    }
    if (pollAbort) {
      pollAbort.abort();
      pollAbort = null;
    }
  }

  $('sw_wifi').addEventListener('change', (ev) => {
    const next = ev.target.checked;
    if (!next && !window.confirm('關閉區網 Wi-Fi 將中斷目前網頁連線，確定要關閉嗎？')) {
      ev.target.checked = true;
      return;
    }
    sendControl('set_wifi', next ? 1 : 0);
  });

  $('sw_cloud').addEventListener('change', (ev) => {
    sendControl('set_cloud', ev.target.checked ? 1 : 0);
  });

  document.querySelectorAll('.ctrl-btn').forEach((btn) => {
    btn.addEventListener('click', () => {
      sendControl(btn.dataset.cmd, 1);
    });
  });

  $('cfgForm').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const body = new URLSearchParams(new FormData(ev.target));
    try {
      const r = await fetchWithTimeout('/save', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: body.toString()
      }, 3000);
      const txt = await r.text();
      if (r.ok) {
        showFormMsg(true, '設定已成功寫入 NVS！');
        showToast('設定已儲存', 'ok');
        $('wifi_pass').value = '';
        loadConfig();
        loadStatus();
      } else {
        showFormMsg(false, '儲存失敗：' + txt);
      }
    } catch (_) {
      showFormMsg(false, '網路連線中斷，請確認裝置熱點狀態');
    }
  });

  document.addEventListener('visibilitychange', () => {
    if (document.hidden) {
      stopPolling();
    } else {
      loadStatus();
      startPolling();
    }
  });

  loadConfig();
  loadStatus();
  startPolling();
})();