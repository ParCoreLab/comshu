// What the model is allowed to draw.
//
// A chart arrives as { sql, spec }: a Vega-Lite spec with NO data, plus the
// query that fills it. The page runs the SQL through its own `query()` and
// binds the rows in. That split is the reason this file exists — a spec that
// can name its own data source has quietly become a second data path, and the
// derived layer forks the moment two panels disagree about total bytes.
//
// So the guard is not mainly about safety (the page is loopback and the user
// could run any query themselves). It is about keeping every chart traceable to
// one visible query. `data.url`, inline `values` and `datasets` are refused for
// that reason first, and the sandbox reason second.

const MARKS = new Set([
  'bar', 'line', 'point', 'circle', 'square', 'rect', 'area', 'tick', 'rule', 'text', 'arc', 'boxplot',
]);

const CHANNELS = new Set([
  'x', 'y', 'x2', 'y2', 'xOffset', 'yOffset', 'color', 'size', 'shape', 'opacity', 'strokeWidth',
  'tooltip', 'text', 'row', 'column', 'theta', 'radius', 'detail', 'order', 'href',
]);

const CHANNEL_KEYS = new Set([
  'field', 'type', 'aggregate', 'bin', 'timeUnit', 'scale', 'axis', 'legend', 'title', 'sort',
  'stack', 'value', 'format', 'formatType', 'band', 'impute',
]);

const TRANSFORMS = new Set(['filter', 'calculate', 'aggregate', 'bin', 'timeUnit', 'fold', 'stack', 'window', 'joinaggregate']);

const VIEW_KEYS = new Set(['mark', 'encoding', 'transform', 'title', 'width', 'height', 'data', 'layer', 'description']);

// Refused wherever they appear. `params`/`selection` bring interaction state we
// have nowhere to put; the rest are data sources or escape hatches.
const BANNED_KEYS = new Set([
  'url', 'values', 'datasets', 'params', 'selection', 'signal', 'config', 'usermeta', 'loader',
  'sequence', 'graticule', 'sphere', 'projection', 'resolve', 'spec', 'repeat', 'facet', 'concat',
  'hconcat', 'vconcat',
]);

const MARK_TYPES = new Set(['bar', 'line', 'point', 'circle', 'square', 'rect', 'area', 'tick', 'rule', 'text', 'arc', 'boxplot']);

function walkBanned(node, path, errors) {
  if (Array.isArray(node)) {
    node.forEach((v, i) => walkBanned(v, `${path}[${i}]`, errors));
    return;
  }
  if (!node || typeof node !== 'object') return;
  for (const [k, v] of Object.entries(node)) {
    if (BANNED_KEYS.has(k)) {
      errors.push(
        k === 'url' || k === 'values' || k === 'datasets'
          ? `${path}.${k}: a chart may not carry or fetch its own data — put the query in \`sql\` and leave \`data\` out of the spec`
          : `${path}.${k}: not supported in this renderer`,
      );
      continue;
    }
    walkBanned(v, `${path}.${k}`, errors);
  }
}

function checkView(view, path, errors) {
  if (!view || typeof view !== 'object' || Array.isArray(view)) {
    errors.push(`${path}: expected an object`);
    return;
  }
  for (const k of Object.keys(view)) {
    if (!VIEW_KEYS.has(k) && !BANNED_KEYS.has(k)) {
      errors.push(`${path}.${k}: unknown key (allowed: ${[...VIEW_KEYS].join(', ')})`);
    }
  }

  if (Array.isArray(view.layer)) {
    if (view.layer.length > 4) errors.push(`${path}.layer: at most 4 layers`);
    view.layer.forEach((v, i) => checkView(v, `${path}.layer[${i}]`, errors));
  } else if (view.layer !== undefined) {
    errors.push(`${path}.layer: expected an array`);
  } else if (view.mark === undefined) {
    errors.push(`${path}: a view needs a \`mark\` (or a \`layer\` of views)`);
  }

  if (view.mark !== undefined) {
    const type = typeof view.mark === 'string' ? view.mark : view.mark?.type;
    if (!MARKS.has(type)) {
      errors.push(`${path}.mark: \`${JSON.stringify(view.mark)}\` — use one of: ${[...MARK_TYPES].join(', ')}`);
    }
  }

  if (view.encoding !== undefined) {
    if (typeof view.encoding !== 'object' || Array.isArray(view.encoding)) {
      errors.push(`${path}.encoding: expected an object`);
    } else {
      for (const [ch, def] of Object.entries(view.encoding)) {
        if (!CHANNELS.has(ch)) {
          errors.push(`${path}.encoding.${ch}: unknown channel (allowed: ${[...CHANNELS].join(', ')})`);
          continue;
        }
        const defs = Array.isArray(def) ? def : [def];
        for (const d of defs) {
          if (!d || typeof d !== 'object') continue;
          for (const k of Object.keys(d)) {
            if (!CHANNEL_KEYS.has(k) && !BANNED_KEYS.has(k)) {
              errors.push(`${path}.encoding.${ch}.${k}: unknown key (allowed: ${[...CHANNEL_KEYS].join(', ')})`);
            }
          }
          if (d.type && !['quantitative', 'ordinal', 'nominal', 'temporal'].includes(d.type)) {
            errors.push(`${path}.encoding.${ch}.type: \`${d.type}\` — use quantitative, ordinal, nominal or temporal`);
          }
        }
      }
    }
  }

  if (view.transform !== undefined) {
    if (!Array.isArray(view.transform)) {
      errors.push(`${path}.transform: expected an array`);
    } else {
      view.transform.forEach((t, i) => {
        const kinds = Object.keys(t ?? {}).filter((k) => TRANSFORMS.has(k));
        if (!kinds.length) {
          errors.push(
            `${path}.transform[${i}]: unsupported transform — allowed: ${[...TRANSFORMS].join(', ')}. ` +
              'Aggregation is usually better done in the SQL anyway.',
          );
        }
      });
    }
  }

  if (view.data !== undefined) {
    const ok = typeof view.data === 'object' && view.data !== null && Object.keys(view.data).every((k) => k === 'name');
    if (!ok || (view.data.name !== undefined && view.data.name !== 'rows')) {
      errors.push(`${path}.data: leave it out — the rows come from \`sql\`. (\`{"name": "rows"}\` is the only accepted value.)`);
    }
  }
}

/**
 * Validate and normalise. Returns { ok, errors, spec }.
 * On success `spec` is what the page should render: data named, schema stamped.
 */
export function guardSpec(spec) {
  const errors = [];
  if (typeof spec === 'string') {
    try {
      spec = JSON.parse(spec);
    } catch (e) {
      return { ok: false, errors: [`spec is not valid JSON: ${e.message}`], spec: null };
    }
  }
  walkBanned(spec, 'spec', errors);
  checkView(spec, 'spec', errors);
  if (errors.length) return { ok: false, errors, spec: null };

  const out = {
    $schema: 'https://vega.github.io/schema/vega-lite/v6.json',
    ...spec,
    data: { name: 'rows' },
  };
  return { ok: true, errors: [], spec: out };
}

/**
 * Fields the spec expects to find in the rows, so a chart can be checked
 * against its own query before anyone looks at an empty plot.
 */
export function specFields(spec) {
  const fields = new Set();
  const walk = (n) => {
    if (Array.isArray(n)) return n.forEach(walk);
    if (!n || typeof n !== 'object') return;
    for (const [k, v] of Object.entries(n)) {
      if (k === 'field' && typeof v === 'string') fields.add(v);
      else walk(v);
    }
  };
  walk(spec);
  return [...fields];
}
