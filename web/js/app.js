(function () {
  'use strict';

  var call = window.Bitflow.call;
  var rows = new Map();
  var samples = new Map();
  var view = {
    filter: 'all',
    paused: false,
    info: {},
    counts: { all: 0, active: 0, completed: 0, failed: 0 },
    lastPath: ''
  };

  var listEl = document.getElementById('list');
  var emptyEl = document.getElementById('empty');
  var tpl = document.getElementById('row-template');
  var toastEl = document.getElementById('toast');
  var dialog = document.getElementById('add-dialog');
  var form = document.getElementById('add-form');
  var errEl = document.getElementById('dialog-error');
  var btnPause = document.getElementById('btn-pause');
  var btnResume = document.getElementById('btn-resume');

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
    if (item.state === 'completed' || item.state === 'failed' || item.state === 'cancelled') speed = 0;
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
      var path = el.dataset.path || '';
      call('shell.reveal', { path: path }).catch(function (error) {
        showToast(error.message);
      });
    });

    listEl.appendChild(el);
    rows.set(item.id, el);
    return el;
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

    var canOpen = item.state === 'completed';
    el.querySelector('.act-open').hidden = !canOpen;
    el.querySelector('.act-cancel').hidden = !(item.state === 'queued' || item.state === 'downloading' || item.state === 'verifying');

    el.hidden = !matches(item);
  }

  function refreshNav() {
    document.querySelectorAll('[data-count]').forEach(function (node) {
      node.textContent = String(view.counts[node.dataset.count] || 0);
    });
    document.querySelectorAll('.nav-item').forEach(function (node) {
      node.classList.toggle('is-active', node.dataset.filter === view.filter);
    });
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
      if (item.path) view.lastPath = item.path;
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
    var dir = view.info.downloads || '';
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
        setFilter(node.dataset.filter);
      });
    });

    form.addEventListener('submit', submitForm);
    document.getElementById('f-url').addEventListener('input', suggestPath);
    dialog.addEventListener('close', function () {
      errEl.hidden = true;
    });

    document.addEventListener('keydown', function (event) {
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'n') {
        event.preventDefault();
        if (!dialog.open) openDialog();
      }
      if (event.key === 'Escape' && dialog.open) dialog.close();
    });
  }

  function boot() {
    bindUi();

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
      })
      .catch(function (error) {
        showToast(error.message);
      });
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', boot);
  } else {
    boot();
  }
})();
