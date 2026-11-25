(function () {
  'use strict';

  var host = (window.chrome && window.chrome.webview) ? window.chrome.webview : null;
  var pending = new Map();
  var listeners = new Set();
  var seq = 0;

  function handleMessage(raw) {
    var msg = raw;
    if (typeof msg === 'string') {
      try {
        msg = JSON.parse(msg);
      } catch (e) {
        return;
      }
    }
    if (!msg || !msg.type) return;

    if (msg.type === 'reply') {
      var entry = pending.get(msg.rid);
      if (!entry) return;
      pending.delete(msg.rid);
      clearTimeout(entry.timer);
      if (msg.ok) entry.resolve(msg.data);
      else entry.reject(new Error(msg.error || '宿主返回失败'));
      return;
    }

    listeners.forEach(function (fn) {
      try {
        fn(msg);
      } catch (e) {
        console.error(e);
      }
    });
  }

  if (host) {
    host.addEventListener('message', function (event) {
      handleMessage(event.data);
    });
  }

  function call(cmd, payload) {
    return new Promise(function (resolve, reject) {
      if (!host) {
        reject(new Error('当前页面不在 BitFlow 宿主中运行'));
        return;
      }
      var rid = String(++seq);
      var timer = setTimeout(function () {
        if (pending.delete(rid)) reject(new Error('宿主响应超时'));
      }, 8000);
      pending.set(rid, { resolve: resolve, reject: reject, timer: timer });

      var body = Object.assign({ rid: rid, cmd: cmd }, payload || {});
      host.postMessage(body);
    });
  }

  function on(fn) {
    listeners.add(fn);
    return function () {
      listeners.delete(fn);
    };
  }

  window.Bitflow = {
    call: call,
    on: on,
    available: !!host
  };
})();
