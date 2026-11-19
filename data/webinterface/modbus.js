/* Flow.io Modbus TCP configuration: FR/EN, three ports, no client-side framework. */
(() => {
  'use strict';
  const page = document.getElementById('page-modbus');
  const grid = document.getElementById('modbusPortCards');
  const status = document.getElementById('modbusStatus');
  if (!page || !grid || !status) return;

  const labels = {
    fr: {
      loading: 'Chargement des paramètres Modbus…',
      loadError: 'Impossible de lire les paramètres Modbus.',
      saveError: 'Échec de l’enregistrement',
      saved: 'Enregistré dans Flow.io. Redémarrez pour activer les modifications.',
      enabled: 'Activer ce port',
      source: 'Paramètres RS485',
      device: 'Équipement Flow.io (adresse RS485 fixe)',
      manual: 'Configuration série manuelle',
      selectDevice: 'Équipement RS485',
      noDevice: 'Aucun équipement RS485 disponible',
      baud: 'Débit (baud)',
      dataBits: 'Bits de données : 8 (fixe)',
      parity: 'Parité',
      stopBits: 'Bits d’arrêt',
      none: 'Aucune',
      even: 'Paire',
      odd: 'Impaire',
      timeout: 'Timeout TCP (ms), attente file RS485 incluse',
      error: 'En cas d’erreur',
      exception: 'Répondre avec une exception Modbus standard',
      silence: 'Rester muet (connexion conservée)',
      save: 'Enregistrer ce port',
      refresh: 'Actualiser',
      clients: 'Clients',
      requests: 'Requêtes',
      errors: 'Exceptions',
      delay: 'Latence TCP max',
      queue: 'Attente RS485 max',
      queued: 'File RTU',
      peak: 'pic',
      rejected: 'rejets',
      response: 'Réponse RTU max',
      active: 'Passerelle active',
      inactive: 'Passerelle non active',
      restart: 'Redémarrage nécessaire après modification',
      invalid: 'Vérifiez les paramètres de ce port.'
    },
    en: {
      loading: 'Loading Modbus settings…',
      loadError: 'Cannot load Modbus settings.',
      saveError: 'Failed to save',
      saved: 'Saved in Flow.io. Restart to apply the changes.',
      enabled: 'Enable this port',
      source: 'RS485 settings',
      device: 'Flow.io equipment (fixed RTU address)',
      manual: 'Manual serial configuration',
      selectDevice: 'RS485 equipment',
      noDevice: 'No RS485 equipment available',
      baud: 'Baud rate',
      dataBits: 'Data bits: 8 (fixed)',
      parity: 'Parity',
      stopBits: 'Stop bits',
      none: 'None',
      even: 'Even',
      odd: 'Odd',
      timeout: 'TCP timeout (ms), including RS485 queue wait',
      error: 'On error',
      exception: 'Return standard Modbus exception',
      silence: 'Remain silent (keep connection open)',
      save: 'Save this port',
      refresh: 'Refresh',
      clients: 'Clients',
      requests: 'Requests',
      errors: 'Exceptions',
      delay: 'Max TCP latency',
      queue: 'Max RS485 queue wait',
      queued: 'RTU queue',
      peak: 'peak',
      rejected: 'rejected',
      response: 'Max RTU response',
      active: 'Gateway active',
      inactive: 'Gateway not active',
      restart: 'Restart needed after changing settings',
      invalid: 'Check this port configuration.'
    }
  };
  const lang = () => document.documentElement.lang.toLowerCase().startsWith('en') ? 'en' : 'fr';
  const t = (key) => labels[lang()][key] || key;
  let latest = null;
  let loading = false;

  const style = document.createElement('style');
  style.textContent = [
    '#page-modbus .modbus-port-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,325px),1fr));gap:16px}',
    '#page-modbus .modbus-toolbar{grid-column:1/-1;display:flex;align-items:center;justify-content:space-between;gap:12px}',
    '#page-modbus .modbus-stat-line{font-size:13px;opacity:.8;margin:0 0 12px}',
    '#page-modbus .modbus-port-card{padding:18px;border:1px solid rgba(120,130,145,.35);border-radius:14px;min-width:0}',
    '#page-modbus .modbus-port-card h3{margin:0 0 14px;font-size:18px}',
    '#page-modbus .modbus-fields{display:grid;gap:12px}',
    '#page-modbus .modbus-field{display:grid;gap:5px;font-size:14px}',
    '#page-modbus .modbus-field input:not([type=checkbox]),#page-modbus .modbus-field select{box-sizing:border-box;width:100%;min-height:38px;padding:7px;border:1px solid #8d98a7;border-radius:8px;background:var(--md-sys-color-surface,transparent);color:inherit}',
    '#page-modbus .modbus-field input[type=checkbox]{margin-right:9px}',
    '#page-modbus .modbus-port-card button{margin-top:14px;min-height:38px}',
    '#page-modbus [hidden]{display:none!important}'
  ].join('');
  document.head.appendChild(style);

  function field(form, title, name, type, value, options, attrs) {
    const label = document.createElement('label');
    label.className = 'modbus-field';
    const span = document.createElement('span');
    span.textContent = title;
    label.appendChild(span);
    const input = document.createElement(type === 'select' ? 'select' : 'input');
    input.name = name;
    if (type === 'select') {
      for (const option of options || []) {
        const item = document.createElement('option');
        item.value = String(option.value);
        item.textContent = String(option.title);
        input.appendChild(item);
      }
      input.value = String(value);
    } else {
      input.type = type;
      if (type === 'checkbox') {
        input.checked = !!value;
      } else {
        input.value = String(value);
      }
    }
    if (attrs) {
      for (const [key, val] of Object.entries(attrs)) input.setAttribute(key, val);
    }
    label.appendChild(input);
    form.appendChild(label);
    return input;
  }

  function cardForPort(cfg, devices) {
    const form = document.createElement('form');
    form.className = 'modbus-port-card';
    const heading = document.createElement('h3');
    heading.textContent = 'TCP ' + cfg.port;
    form.appendChild(heading);
    const stats = cfg.stats || {};
    const summary = document.createElement('p');
    summary.className = 'modbus-stat-line';
    summary.textContent =
      t('clients') + ': ' + (stats.clients || 0) + '/2 · ' +
      t('requests') + ': ' + (stats.requests || 0) + ' · ' +
      t('errors') + ': ' + (stats.exceptions || 0) + ' · ' +
      t('delay') + ': ' + (stats.tcp_max_ms || 0) + ' ms' + ' · ' +
      t('queue') + ': ' + (stats.queue_max_ms || 0) + ' ms' + ' · ' +
      t('response') + ': ' + (stats.rtu_max_ms || 0) + ' ms';
    form.appendChild(summary);
    const fields = document.createElement('div');
    fields.className = 'modbus-fields';
    form.appendChild(fields);

    const enable = field(fields, t('enabled'), 'enabled', 'checkbox', cfg.enabled);
    const source = field(fields, t('source'), 'source', 'select', cfg.source, [
      {value: 0, title: t('device')}, {value: 1, title: t('manual')}
    ]);
    const deviceBox = document.createElement('div');
    fields.appendChild(deviceBox);
    const choices = devices.map((d) => ({
      value: d.id, title: d.label + ' (#' + d.id + ', RTU ' + d.rtu_unit_id + ', ' + d.baud + ' baud)'
    }));
    if (!choices.length) {
      choices.push({value: cfg.device_id, title: t('noDevice')});
    } else if (!choices.some((d) => Number(d.value) === cfg.device_id)) {
      choices.push({value: cfg.device_id, title: '#' + cfg.device_id + ' — ' + t('noDevice')});
    }
    field(deviceBox, t('selectDevice'), 'device_id', 'select', cfg.device_id, choices);

    const manualBox = document.createElement('div');
    manualBox.className = 'modbus-fields';
    fields.appendChild(manualBox);
    field(manualBox, t('baud'), 'baud', 'number', cfg.baud, null,
          {min: '1200', max: '115200', step: '1', required: 'required'});
    const bits = document.createElement('span');
    bits.textContent = t('dataBits');
    manualBox.appendChild(bits);
    field(manualBox, t('parity'), 'parity', 'select', cfg.parity, [
      {value: 0, title: t('none')}, {value: 1, title: t('even')}, {value: 2, title: t('odd')}
    ]);
    field(manualBox, t('stopBits'), 'stop_bits', 'select', cfg.stop_bits, [
      {value: 1, title: '1'}, {value: 2, title: '2'}
    ]);
    field(fields, t('timeout'), 'timeout_ms', 'number', cfg.timeout_ms, null,
          {min: '100', max: '120000', step: '1', required: 'required'});
    field(fields, t('error'), 'error_mode', 'select', cfg.error_mode, [
      {value: 0, title: t('exception')}, {value: 1, title: t('silence')}
    ]);

    const updateMode = () => {
      const isManual = Number(source.value) === 1;
      deviceBox.hidden = isManual;
      manualBox.hidden = !isManual;
    };
    source.addEventListener('change', updateMode);
    updateMode();

    const button = document.createElement('button');
    button.className = 'btn-primary';
    button.type = 'submit';
    button.textContent = t('save');
    form.appendChild(button);
    form.addEventListener('submit', async (event) => {
      event.preventDefault();
      if (!form.reportValidity()) {
        status.textContent = t('invalid');
        return;
      }
      const values = new URLSearchParams();
      values.set('index', String(cfg.index));
      values.set('enabled', enable.checked ? '1' : '0');
      for (const key of ['source', 'device_id', 'baud', 'parity', 'stop_bits',
                         'timeout_ms', 'error_mode']) {
        values.set(key, form.elements.namedItem(key).value);
      }
      button.disabled = true;
      try {
        const response = await fetch('/api/modbus/port', {
          method: 'POST', credentials: 'same-origin', body: values
        });
        const data = await response.json().catch(() => ({}));
        if (!response.ok || !data.ok) throw new Error(data.error || response.status);
        status.textContent = t('saved');
        cfg.enabled = enable.checked;
      } catch (error) {
        status.textContent = t('saveError') + ': ' + String(error.message || error);
      } finally {
        button.disabled = false;
      }
    });
    return form;
  }

  function render() {
    if (!latest) return;
    grid.replaceChildren();
    const header = document.createElement('div');
    header.className = 'modbus-toolbar';
    const state = document.createElement('span');
    const sched = latest.scheduler || {};
    state.textContent =
      (latest.running ? t('active') : t('inactive')) + ' · ' +
      t('queued') + ': ' + (sched.queue || 0) + ' (' +
      t('peak') + ': ' + (sched.peak || 0) + ', ' +
      t('rejected') + ': ' + (sched.rejected || 0) + ')';
    header.appendChild(state);
    const refresh = document.createElement('button');
    refresh.type = 'button';
    refresh.textContent = t('refresh');
    refresh.addEventListener('click', load);
    header.appendChild(refresh);
    grid.appendChild(header);
    for (const cfg of latest.ports || []) {
      grid.appendChild(cardForPort(cfg, latest.devices || []));
    }
  }

  async function load() {
    if (loading) return;
    loading = true;
    status.textContent = t('loading');
    try {
      const response = await fetch('/api/modbus/ports', {
        cache: 'no-store', credentials: 'same-origin'
      });
      const data = await response.json();
      if (!response.ok || !data || data.ok !== true) throw new Error(response.status);
      latest = data;
      render();
      status.textContent = '';
    } catch (error) {
      status.textContent = t('loadError') + ' (' + String(error.message || error) + ')';
    } finally {
      loading = false;
    }
  }

  new MutationObserver(() => {
    if (page.classList.contains('active')) load();
  }).observe(page, {attributes: true, attributeFilter: ['class']});
  new MutationObserver(() => {
    if (page.classList.contains('active') && latest) render();
  }).observe(document.documentElement, {attributes: true, attributeFilter: ['lang']});
  if (page.classList.contains('active')) load();
})();
