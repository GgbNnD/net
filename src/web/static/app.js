// P2P 文件传输 Web 界面 - 前端逻辑
(function() {
  'use strict';

  const API = '/api';

  // --- 状态 ---
  let selectedDeviceIp = '';
  let selectedDevicePort = 0;
  const transfers = {};

  // --- 初始化 ---
  function init() {
    refreshDevices();
    setInterval(refreshDevices, 3000);
    setInterval(refreshTransfers, 2000);

    document.getElementById('file-input').addEventListener('change', onFilesSelected);
    document.getElementById('send-btn').addEventListener('click', onSendClick);
  }

  // --- 设备列表 ---
  async function refreshDevices() {
    try {
      const resp = await fetch(API + '/devices');
      const data = await resp.json();
      renderDevices(data.devices || []);
      document.getElementById('device-info').textContent =
        '本机: ' + (data.self_name || '--');
    } catch(e) {
      console.error('Failed to fetch devices:', e);
    }
  }

  function renderDevices(devices) {
    const list = document.getElementById('device-list');
    const count = document.getElementById('online-count');
    const select = document.getElementById('target-device');

    count.textContent = '(' + devices.length + ')';

    if (devices.length === 0) {
      list.innerHTML = '<li class="empty">暂无在线设备</li>';
      select.innerHTML = '<option value="">--- 请先勾选设备 ---</option>';
      return;
    }

    list.innerHTML = '';
    select.innerHTML = '<option value="">--- 选择设备 ---</option>';

    devices.forEach(function(d) {
      const li = document.createElement('li');
      li.innerHTML = '<span class="device-icon"></span>' + d.name + ' <small>(' + d.ip + ')</small>';
      li.onclick = function() {
        selectedDeviceIp = d.ip;
        selectedDevicePort = d.port || 8889;
        document.querySelectorAll('#device-list li').forEach(function(el) { el.classList.remove('selected'); });
        li.classList.add('selected');
        updateSendBtn();
      };
      list.appendChild(li);

      const opt = document.createElement('option');
      opt.value = d.ip + ':' + (d.port || 8889);
      opt.textContent = d.name + ' (' + d.ip + ')';
      select.appendChild(opt);
    });

    select.addEventListener('change', function() {
      const val = select.value;
      if (val) {
        const parts = val.split(':');
        selectedDeviceIp = parts[0];
        selectedDevicePort = parseInt(parts[1]) || 8889;
      } else {
        selectedDeviceIp = '';
        selectedDevicePort = 0;
      }
      updateSendBtn();
    });
  }

  function updateSendBtn() {
    const btn = document.getElementById('send-btn');
    const file = document.getElementById('file-input');
    btn.disabled = !(selectedDeviceIp && file.files.length > 0);
  }

  function onFilesSelected() { updateSendBtn(); }

  // --- 发送 ---
  async function onSendClick() {
    const fileInput = document.getElementById('file-input');
    if (!fileInput.files.length || !selectedDeviceIp) return;

    const file = fileInput.files[0];
    const fd = new FormData();
    fd.append('target_ip', selectedDeviceIp);
    fd.append('target_port', String(selectedDevicePort));
    fd.append('filename', file.name);
    fd.append('file_size', String(file.size));

    try {
      const resp = await fetch(API + '/transfer', { method: 'POST', body: fd });
      const data = await resp.json();
      if (data.success) {
        addTransferItem(data.file_id, file.name, file.size, 'TRANSFERRING');
      }
    } catch(e) {
      console.error('Failed to start transfer:', e);
    }
  }

  function addTransferItem(fileId, name, size, state) {
    const list = document.getElementById('transfer-list');
    if (list.querySelector('.empty')) list.innerHTML = '';

    const div = document.createElement('div');
    div.className = 'transfer-item';
    div.id = 'transfer-' + fileId;
    div.innerHTML =
      '<div class="name">' + name + '</div>' +
      '<div class="size">' + formatSize(size) + '</div>' +
      '<div class="progress-bar"><div class="progress-fill" style="width:0%"></div></div>' +
      '<div class="status">准备传输...</div>';
    list.prepend(div);

    transfers[fileId] = { name, size };
  }

  async function refreshTransfers() {
    try {
      const resp = await fetch(API + '/transfers');
      const data = await resp.json();
      (data.transfers || []).forEach(function(t) {
        const el = document.getElementById('transfer-' + t.file_id);
        if (!el) {
          addTransferItem(t.file_id, t.filename, t.file_size, t.state);
          return;
        }
        const fill = el.querySelector('.progress-fill');
        const status = el.querySelector('.status');
        const pct = t.total_chunks > 0 ? Math.round(t.progress_chunk / t.total_chunks * 100) : 0;
        fill.style.width = pct + '%';
        if (t.state === 'COMPLETED') {
          fill.classList.add('done');
          status.textContent = '已完成';
        } else if (t.state === 'TRANSFERRING') {
          status.textContent = pct + '% · ' + formatSpeed(t.speed || 0);
        } else {
          status.textContent = t.state;
        }
      });
    } catch(e) {}
  }

  function formatSize(bytes) {
    if (bytes < 1024) return bytes + ' B';
    if (bytes < 1048576) return (bytes/1024).toFixed(1) + ' KB';
    if (bytes < 1073741824) return (bytes/1048576).toFixed(1) + ' MB';
    return (bytes/1073741824).toFixed(1) + ' GB';
  }

  function formatSpeed(bps) {
    if (bps <= 0) return '0 B/s';
    return formatSize(bps) + '/s';
  }

  init();
})();
