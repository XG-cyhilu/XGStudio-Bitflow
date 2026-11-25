(function () {
  'use strict';

  var call = window.Bitflow.call;
  var rows = new Map();
  var samples = new Map();
  var hashes = new Map();
  var view = {
    screen: 'tasks',
    filter: 'all',
    paused: false,
    info: {},
    settings: null,
    counts: { all: 0, active: 0, completed: 0, failed: 0 }
  };

  var listEl = document.getElementById('list');
  var tpl = document.getElementById('row-template');
  var toastEl = document.getElementById('toast');
  var dialog = document.getElementById('add-dialog');
  var form = document.getElementById('add-form');
  var errEl = document.getElementById('dialog-error');
  var btnPause = document.getElementById('btn-pause');
  var btnResume = document.getElementById('btn-resume');
  var termEl = document.getElementById('term-output');
  var termInput = document.getElementById('term-input');
  var probeEl = document.getElementById('probe-result');
  var hintEl = document.getElementById('settings-hint');

  var STATE_TEXT = {
    queued: '排队中',
    downloading: '下载中',
    verifying: '校验中',
    completed: '已完成',
    failed: '失败',
    cancelled: '已取消'
  };

  var ACTIVE_STATES = ['queued', 'downloading', 'verifying'];

  function fmtBytes(value) {
    var n = Number(value) || 0;
    if (n < 1024) return Math.round(n) + ' B';
    var units = ['KB', 'MB', 'GB', 'TB'];
    var i = -1;
    do {
      n /= 1024;
      i++;
    } while (n >= 1024 && i < units.length - 1);
    return (n >= 100 ? n.toFixed(0) : n >= 10 ? n.toFixed(1) : n.toFixed(2)) + ' ' + units[i];
  }

  function fmtSpeed(value) {
    return fmtBytes(value) + '/s';
  }

  function baseName(path) {
    var parts = String(path || '').split(/[\\/]/);
    return parts[parts.length - 1] || String(path || '');
  }

  function hostOf(url) {
    try {
      return new URL(url).host;
    } catch (e) {
      return '';
    }
  }

  function shortPath(path) {
    var text = String(path || '');
    if (text.length <= 72) return text;
    return '…' + text.slice(text.length - 70);
  }

  function isZip(path) {
    return /\.zip$/i.test(String(path || ''));
  }

  function showToast(message) {
    toastEl.textContent = message;
    toastEl.hidden = false;
    requestAnimationFrame(function () {
      toastEl.classList.add('show');
    });
    clearTimeout(showToast.timer);
    showToast.timer = setTimeout(function () {
      toastEl.classList.remove('show');
      setTimeout(function () {
        toastEl.hidden = true;
      }, 220);
    }, 3200);
  }

  function taskSpeed(item, now) {
    var prev = samples.get(item.id);
    var speed = 0;
    if (prev && now > prev.t && item.done >= prev.done) {
      speed = ((item.done - prev.done) * 1000) / (now - prev.t);
    }
    if (item.state !== 'downloading' && item.state !== 'verifying') speed = 0;
    var smoothed = prev ? prev.speed * 0.7 + speed * 0.3 : speed;
    if (!prev || Math.abs(smoothed - speed) < 4096) smoothed = speed;
    samples.set(item.id, { done: item.done, t: now, speed: smoothed });
    return smoothed;
  }

  function matches(item) {
    switch (view.filter) {
      case 'active':
        return ACTIVE_STATES.indexOf(item.state) !== -1;
      case 'completed':
        return item.state === 'completed';
      case 'failed':
        return item.state === 'failed' || item.state === 'cancelled';
      default:
        return true;
    }
  }

  function createRow(item) {
    var el = tpl.content.firstElementChild.cloneNode(true);
    el.dataset.task = String(item.id);
    el.querySelector('.file-name').textContent = baseName(item.path);
    el.querySelector('.file-host').textContent = hostOf(item.url);
    el.querySelector('.file-sub').textContent = shortPath(item.path);
    el.querySelector('.file-sub').title = item.path;

    el.querySelector('.act-cancel').addEventListener('click', function () {
      call('dl.cancel', { task: item.id }).catch(function (error) {
        showToast(error.message);
      });
    });

    el.querySelector('.act-open').addEventListener('click', function () {
      call('shell.reveal', { path: el.dataset.path || '' }).catch(function (error) {
        showToast(error.message);
      });
    });

    el.querySelector('.act-extract').addEventListener('click', function () {
      call('task.extract', { task: item.id }).catch(function (error) {
        showToast(error.message);
      });
    });

    listEl.appendChild(el);
    rows.set(item.id, el);
    return el;
  }

  function applyHash(el, id) {
    var chip = el.querySelector('.hash-chip');
    var info = hashes.get(id);
    if (!info || !info.hex) {
      chip.hidden = true;
      return;
    }
    chip.hidden = false;
    chip.textContent = info.algo + ' ' + info.hex.slice(0, 12) + '…';
    chip.title = info.algo.toUpperCase() + ': ' + info.hex;
  }

  function updateRow(item, now, solo) {
    var el = rows.get(item.id) || createRow(item);
    el.dataset.path = item.path;
    el.dataset.state = item.state;

    var pct = 0;
    if (item.size > 0) pct = Math.min(100, (item.done / item.size) * 100);
    else if (item.state === 'completed') pct = 100;

    var bar = el.querySelector('.bar');
    var indeterminate = item.size === 0 && (item.state === 'downloading' || item.state === 'verifying');
    bar.classList.toggle('is-indeterminate', indeterminate);
    el.querySelector('.bar-fill').style.width = indeterminate ? '100%' : pct.toFixed(1) + '%';

    var doneBytes = item.done;
    if (solo && item.size === 0 && solo.done > doneBytes) doneBytes = solo.done;
    el.querySelector('.cell-progress').textContent =
      item.size > 0 ? fmtBytes(doneBytes) + ' / ' + fmtBytes(item.size) : fmtBytes(doneBytes);

    var speed = taskSpeed(item, now);
    if ((item.state === 'downloading' || item.state === 'verifying') && speed < 1 && solo) {
      speed = solo.speed || 0;
    }
    el.querySelector('.cell-speed').textContent =
      item.state === 'downloading' || item.state === 'verifying' ? fmtSpeed(speed) : '—';

    var badge = el.querySelector('.cell-state .badge');
    badge.textContent = STATE_TEXT[item.state] || item.state;

    var completed = item.state === 'completed';
    el.querySelector('.act-open').hidden = !completed;
    el.querySelector('.act-extract').hidden = !(completed && isZip(item.path));
    el.querySelector('.act-cancel').hidden = !(item.state === 'queued' || item.state === 'downloading' || item.state === 'verifying');

    applyHash(el, item.id);

    el.hidden = view.screen !== 'tasks' || !matches(item);
  }

  function refreshNav() {
    document.querySelectorAll('[data-count]').forEach(function (node) {
      node.textContent = String(view.counts[node.dataset.count] || 0);
    });
    document.querySelectorAll('.nav-item').forEach(function (node) {
      var active;
      if (view.screen === 'tasks') {
        active = node.dataset.view === 'tasks' && (node.dataset.filter || 'all') === view.filter;
      } else {
        active = node.dataset.view === view.screen;
      }
      node.classList.toggle('is-active', active);
    });
  }

  function termAtBottom() {
    return termEl.scrollTop + termEl.clientHeight >= termEl.scrollHeight - 24;
  }

  function termScrollBottom() {
    termEl.scrollTop = termEl.scrollHeight;
  }

  function appendTerm(kind, text, level) {
    var stick = termAtBottom();
    var row = document.createElement('div');
    row.className = 'term-line';
    row.dataset.kind = kind || 'out';
    if (level) row.dataset.level = level;
    row.textContent = text;
    termEl.appendChild(row);
    while (termEl.children.length > 1200) termEl.removeChild(termEl.firstChild);
    if (stick) termScrollBottom();
    return row;
  }

  function appendLogRecords(records) {
    if (!records || !records.length) return;
    records.forEach(function (rec) {
      var date = new Date(rec.time || Date.now());
      var stamp = String(date.getHours()).padStart(2, '0') + ':' +
        String(date.getMinutes()).padStart(2, '0') + ':' +
        String(date.getSeconds()).padStart(2, '0');
      var level = String(rec.level || 'info');
      appendTerm('log', stamp + '  ' + level.toUpperCase().padEnd(5, ' ') + ' ' + (rec.text || ''), level);
    });
  }

  var history = [];
  var historyIndex = 0;

  function runCommand(line) {
    var text = String(line || '').trim();
    if (!text) return;
    appendTerm('echo', '❯ ' + text);
    if (text.toLowerCase() === 'clear' || text.toLowerCase() === 'cls') {
      termEl.innerHTML = '';
      return;
    }
    history.push(text);
    historyIndex = history.length;
    call('term.exec', { line: text })
      .catch(function (error) {
        appendTerm('err', error.message);
      });
  }

  function onTermKey(event) {
    if (event.key === 'Enter') {
      event.preventDefault();
      runCommand(termInput.value);
      termInput.value = '';
      return;
    }
    if (event.key === 'ArrowUp') {
      if (!history.length) return;
      event.preventDefault();
      historyIndex = Math.max(0, historyIndex - 1);
      termInput.value = history[historyIndex] || '';
      return;
    }
    if (event.key === 'ArrowDown') {
      if (!history.length) return;
      event.preventDefault();
      historyIndex = Math.min(history.length, historyIndex + 1);
      termInput.value = historyIndex >= history.length ? '' : history[historyIndex];
      return;
    }
    if (event.key === 'l' && event.ctrlKey) {
      event.preventDefault();
      termEl.innerHTML = '';
    }
  }

  function handleAsync(msg) {
    if (msg.type === 'term') {
      (msg.lines || []).forEach(function (item) {
        if (item.kind === 'clear') {
          termEl.innerHTML = '';
          return;
        }
        appendTerm(item.kind || 'out', item.text || '');
      });
      return;
    }
    if (msg.type === 'verify') {
      hashes.set(msg.task, { algo: msg.algo || 'sha256', hex: msg.hex || '', ok: msg.ok });
      var el = rows.get(msg.task);
      if (el) applyHash(el, msg.task);
      return;
    }
    if (msg.type === 'extract') {
      if (msg.ok) {
        showToast('解压完成：' + String(msg.dest || '').split(/[\\/]/).pop() + '（' + (msg.files || 0) + ' 个文件）');
      } else {
        showToast('解压失败：' + (msg.error || '未知错误'));
      }
      return;
    }
    if (msg.type === 'probe') {
      var speed = msg.speed ? (msg.speed / 1048576).toFixed(2) + ' MB/s' : '—';
      if (msg.ok && msg.partial) {
        probeEl.textContent = '已收 ' + fmtBytes(msg.received) + ' · ' + speed +
          '（15 秒内未读完，按已收字节估算）';
      } else {
        probeEl.textContent = msg.ok
          ? 'HTTP ' + Math.round(msg.status) + ' · ' + Math.round(msg.ms) + ' ms · ' + speed + ' · ' + fmtBytes(msg.received)
          : '失败：' + (msg.error || ('HTTP ' + Math.round(msg.status)));
      }
    }
  }

  function renderTick(msg) {
    var now = performance.now();
    var items = msg.items || [];
    var alive = new Set();

    view.counts = { all: 0, active: 0, completed: 0, failed: 0 };

    var progress = msg.progress || {};
    var solo = null;

    items.forEach(function (item) {
      alive.add(item.id);
      view.counts.all++;
      if (ACTIVE_STATES.indexOf(item.state) !== -1) view.counts.active++;
      if (item.state === 'completed') view.counts.completed++;
      if (item.state === 'failed' || item.state === 'cancelled') view.counts.failed++;
    });

    if (view.counts.active === 1) solo = progress;

    items.forEach(function (item) {
      updateRow(item, now, solo);
    });

    rows.forEach(function (el, id) {
      if (!alive.has(id)) {
        el.remove();
        rows.delete(id);
        samples.delete(id);
      }
    });

    document.body.classList.toggle('has-items', rows.size > 0);

    document.getElementById('speed').textContent = fmtSpeed(progress.speed || 0);
    document.getElementById('stat-queue').textContent = '排队 ' + (progress.queued || 0);
    document.getElementById('stat-active').textContent = '活动 ' + (progress.active || 0);
    document.getElementById('stat-total').textContent = '已下载 ' + fmtBytes(progress.done || 0);

    if (typeof progress.paused === 'boolean') {
      view.paused = progress.paused;
      btnPause.disabled = view.paused;
      btnResume.disabled = !view.paused;
    }

    if (msg.events && msg.events.length) {
      msg.events.forEach(function (event) {
        if (event.kind === 'completed') {
          showToast(baseName(event.path) + ' 下载完成');
        } else {
          showToast(baseName(event.path) + ' 下载失败' + (event.message ? '：' + event.message : ''));
        }
      });
    }

    if (msg.logs) appendLogRecords(msg.logs);
    if (msg.async) msg.async.forEach(handleAsync);

    refreshNav();
  }

  function setFilter(filter) {
    view.filter = filter;
    document.querySelectorAll('.row').forEach(function (el) {
      var state = el.dataset.state;
      var keep =
        filter === 'all' ||
        (filter === 'active' && ACTIVE_STATES.indexOf(state) !== -1) ||
        (filter === 'completed' && state === 'completed') ||
        (filter === 'failed' && (state === 'failed' || state === 'cancelled'));
      el.hidden = !keep;
    });
    refreshNav();
  }

  function setScreen(screen) {
    view.screen = screen;
    document.querySelectorAll('.view').forEach(function (node) {
      node.hidden = node.dataset.view !== screen;
    });
    if (screen === 'settings') loadSettings();
    if (screen === 'term') {
      termScrollBottom();
      termInput.focus();
    }
    refreshNav();
  }

  function openDialog() {
    errEl.hidden = true;
    form.reset();
    dialog.showModal();
    setTimeout(function () {
      document.getElementById('f-url').focus();
    }, 30);
  }

  function suggestPath() {
    var url = document.getElementById('f-url').value.trim();
    var path = document.getElementById('f-path');
    if (!url || path.value) return;
    var name = 'download.bin';
    try {
      var parsed = new URL(url);
      var seg = parsed.pathname.split('/').filter(Boolean).pop();
      if (seg) name = decodeURIComponent(seg);
    } catch (e) {
      /* 非法 URL，保留默认名 */
    }
    var dir = (view.settings && view.settings.downloadDir) || view.info.downloads || '';
    path.value = dir ? dir + '\\' + name : name;
  }

  function submitForm(event) {
    event.preventDefault();
    errEl.hidden = true;

    var url = document.getElementById('f-url').value.trim();
    var path = document.getElementById('f-path').value.trim();
    if (!url || !path) {
      errEl.textContent = '下载地址和保存路径都不能为空。';
      errEl.hidden = false;
      return;
    }

    var sizeText = document.getElementById('f-size').value.replace(/[^0-9]/g, '');
    var algo = document.getElementById('f-algo').value;
    var digest = document.getElementById('f-digest').value.trim();
    var mirrors = document.getElementById('f-mirrors').value
      .split('\n')
      .map(function (line) { return line.trim(); })
      .filter(Boolean);

    var payload = {
      url: url,
      path: path,
      size: sizeText ? Number(sizeText) : 0,
      priority: document.getElementById('f-priority').value,
      algo: algo,
      digest: algo === 'none' ? '' : digest
    };
    if (mirrors.length) payload.mirrors = mirrors;

    call('dl.add', payload)
      .then(function () {
        dialog.close();
        setScreen('tasks');
        setFilter('all');
      })
      .catch(function (error) {
        errEl.textContent = error.message;
        errEl.hidden = false;
      });
  }

  function togglePause(paused) {
    var cmd = paused ? 'engine.pause' : 'engine.resume';
    call(cmd)
      .then(function () {
        view.paused = paused;
        btnPause.disabled = paused;
        btnResume.disabled = !paused;
        showToast(paused ? '已暂停派发新任务' : '已恢复下载');
      })
      .catch(function (error) {
        showToast(error.message);
      });
  }

  function fillSettings(s) {
    document.getElementById('s-downloadDir').value = s.downloadDir || '';
    document.getElementById('s-smallThreads').value = s.smallThreads;
    document.getElementById('s-largeThreads').value = s.largeThreads;
    document.getElementById('s-maxConnections').value = s.maxConnections;
    document.getElementById('s-speedLimitKb').value = s.speedLimitKb;
    document.getElementById('s-logLevel').value = s.logLevel || 'debug';
    document.getElementById('s-autoExtract').checked = !!s.autoExtract;
    document.getElementById('s-probeUrl').value = s.probeUrl || '';
    document.getElementById('about-installId').textContent = s.installId || '—';
    document.getElementById('about-logDir').textContent = s.logDir || '—';
    document.getElementById('about-settingsPath').textContent = s.settingsPath || '—';
  }

  function loadSettings() {
    call('settings.get')
      .then(function (s) {
        view.settings = s;
        fillSettings(s);
      })
      .catch(function (error) {
        showToast(error.message);
      });
  }

  function numField(id, fallback) {
    var raw = document.getElementById(id).value.replace(/[^0-9]/g, '');
    if (!raw) return fallback;
    return Number(raw);
  }

  function saveSettings() {
    if (!view.settings) {
      showToast('设置尚未加载');
      return;
    }
    var payload = {
      downloadDir: document.getElementById('s-downloadDir').value.trim(),
      smallThreads: numField('s-smallThreads', 16),
      largeThreads: numField('s-largeThreads', 4),
      maxConnections: numField('s-maxConnections', 48),
      speedLimitKb: numField('s-speedLimitKb', 0),
      logLevel: document.getElementById('s-logLevel').value,
      autoExtract: document.getElementById('s-autoExtract').checked,
      probeUrl: document.getElementById('s-probeUrl').value.trim()
    };

    call('settings.set', payload)
      .then(function (res) {
        view.settings = Object.assign({}, view.settings, payload);
        hintEl.hidden = false;
        hintEl.textContent = res && res.restartRequired
          ? '已保存。引擎参数（线程 / 连接数 / 限速）需要重启 BitFlow 后生效。'
          : '已保存到 settings.json。';
        showToast('设置已保存');
      })
      .catch(function (error) {
        showToast(error.message);
      });
  }

  function runProbe() {
    probeEl.textContent = '正在测速…';
    var url = document.getElementById('s-probeUrl').value.trim();
    call('net.probe', url ? { url: url } : {})
      .catch(function (error) {
        probeEl.textContent = '失败：' + error.message;
      });
  }

  function bindUi() {
    document.getElementById('btn-add').addEventListener('click', openDialog);
    document.getElementById('btn-empty-add').addEventListener('click', openDialog);
    document.getElementById('btn-cancel-add').addEventListener('click', function () {
      dialog.close();
    });

    btnPause.addEventListener('click', function () {
      togglePause(true);
    });
    btnResume.addEventListener('click', function () {
      togglePause(false);
    });

    document.getElementById('btn-cancelAll').addEventListener('click', function () {
      call('dl.cancelAll').then(function () {
        showToast('已取消队列中的所有任务');
      }).catch(function (error) {
        showToast(error.message);
      });
    });

    document.querySelectorAll('.nav-item').forEach(function (node) {
      node.addEventListener('click', function () {
        if (node.dataset.view === 'tasks' && node.dataset.filter) setFilter(node.dataset.filter);
        setScreen(node.dataset.view || 'tasks');
      });
    });

    document.getElementById('btn-clear-log').addEventListener('click', function () {
      termEl.innerHTML = '';
    });

    document.getElementById('btn-open-logdir').addEventListener('click', function () {
      call('log.revealDir').catch(function (error) {
        showToast(error.message);
      });
    });

    document.getElementById('btn-save-settings').addEventListener('click', saveSettings);
    document.getElementById('btn-probe').addEventListener('click', runProbe);

    form.addEventListener('submit', submitForm);
    document.getElementById('f-url').addEventListener('input', suggestPath);
    dialog.addEventListener('close', function () {
      errEl.hidden = true;
    });

    termInput.addEventListener('keydown', onTermKey);
    document.getElementById('term-input-row').addEventListener('click', function () {
      termInput.focus();
    });
    termEl.addEventListener('click', function () {
      if (!window.getSelection().toString()) termInput.focus();
    });

    document.addEventListener('keydown', function (event) {
      if (termInput === document.activeElement) return;
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'n') {
        event.preventDefault();
        if (!dialog.open) openDialog();
      }
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'l' && view.screen === 'term') {
        event.preventDefault();
        termEl.innerHTML = '';
      }
      if (event.key === 'Escape' && dialog.open) dialog.close();
    });
  }

  function boot() {
    bindUi();

    appendTerm('banner', 'BitFlow terminal · xgcfg / xgcrypto / xgfs / xglog / xgnet / xgproc / xgsecure / xgzip');
    appendTerm('info', '输入 help 查看命令，↑↓ 翻历史，Ctrl+L 清屏。');

    window.Bitflow.on(function (msg) {
      if (msg.type === 'tick') renderTick(msg);
      else if (msg.type === 'theme') document.documentElement.dataset.theme = msg.theme;
    });

    call('app.info')
      .then(function (info) {
        view.info = info || {};
        document.documentElement.dataset.theme = info.theme || 'light';
        document.getElementById('stat-version').textContent = 'BitFlow ' + (info.version || '');
        document.getElementById('engine-label').textContent = (info.engine || 'XGDL') + ' 引擎';
        document.getElementById('about-version').textContent = 'BitFlow ' + (info.version || '');
        document.getElementById('about-engine').textContent = (info.engine || 'XGDL');
      })
      .catch(function (error) {
        showToast(error.message);
      });

    loadSettings();
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', boot);
  } else {
    boot();
  }
})();
