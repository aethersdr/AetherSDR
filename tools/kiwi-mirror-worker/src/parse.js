/**
 * Parse the KiwiSDR public receiver directory (HTML) into structured JSON.
 *
 * The directory is a page of `<div class='cl-entry seq_N snr_all_N snr_hf_N'>`
 * blocks. Each carries a run of `<!-- key=value -->` comments holding the real
 * data, plus two anchors to the receiver (an avatar link and a text link) and a
 * `cl-name` div. We take the data from the comments rather than the rendered
 * markup: `cl-name` is upper-cased for display, while the `name` comment keeps
 * the operator's own capitalization.
 *
 * Everything the source sends is preserved. Fields are typed only where the
 * meaning is unambiguous; anything we are not certain about stays a string, so
 * a format change upstream degrades to "odd-looking string" rather than
 * "silently wrong number".
 */

/** Values that are plain integers. */
const INT_FIELDS = new Set([
  'users', 'users_max', 'asl', 'uptime', 'fixes', 'fixes_min', 'fixes_hour',
  'adc_ov', 'avatar_ctime', 'ext_api', 'preempt', 'gps_good', 'sm_cal',
  'wf_cal', 'clk_ext_freq', 'tdoa_ch',
]);

/** Values that are decimals. */
const FLOAT_FIELDS = new Set(['freq_offset']);

/** `yes`/`no` values. */
const BOOL_FIELDS = new Set(['offline']);

/** `0`/`1` values. */
const BIT_FIELDS = new Set(['ant_connected']);

/** Comma-separated integer lists whose length we do not want to assume. */
const INT_LIST_FIELDS = new Set(['snr', 'clk_ext_gps', 'gps_date']);

const ENTITIES = {
  '&amp;': '&', '&lt;': '<', '&gt;': '>', '&quot;': '"',
  '&#39;': "'", '&apos;': "'", '&nbsp;': ' ',
};

function decodeEntities(s) {
  return s
    .replace(/&#(\d+);/g, (_, d) => String.fromCodePoint(Number(d)))
    .replace(/&#x([0-9a-f]+);/gi, (_, h) => String.fromCodePoint(parseInt(h, 16)))
    .replace(/&(amp|lt|gt|quot|#39|apos|nbsp);/g, (m) => ENTITIES[m] ?? m);
}

function toInt(v) {
  // Reject anything that is not cleanly an integer so a changed format shows up
  // as the original string instead of a NaN or a silently truncated number.
  return /^-?\d+$/.test(v) ? Number(v) : v;
}

function toFloat(v) {
  return /^-?\d+(\.\d+)?$/.test(v) ? Number(v) : v;
}

/** `(-34.964437, 138.762380)` -> `[-34.964437, 138.76238]` */
function parseGps(v) {
  const m = v.match(/^\(\s*(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)\s*\)$/);
  return m ? [Number(m[1]), Number(m[2])] : v;
}

/** `0-30000000` -> `[0, 30000000]`; multiple ranges are kept as a list. */
function parseBands(v) {
  const parts = v.split(',').map((p) => p.trim()).filter(Boolean);
  const ranges = parts.map((p) => {
    const m = p.match(/^(\d+)-(\d+)$/);
    return m ? [Number(m[1]), Number(m[2])] : null;
  });
  if (ranges.some((r) => r === null)) return v;
  return ranges.length === 1 ? ranges[0] : ranges;
}

function parseIntList(v) {
  if (v === '') return [];
  const parts = v.split(',').map((p) => p.trim());
  return parts.every((p) => /^-?\d+$/.test(p)) ? parts.map(Number) : v;
}

function coerce(key, raw) {
  const v = decodeEntities(raw).trim();
  if (v === '') return '';
  if (key === 'gps') return parseGps(v);
  if (key === 'bands') return parseBands(v);
  if (INT_LIST_FIELDS.has(key)) return parseIntList(v);
  if (INT_FIELDS.has(key)) return toInt(v);
  if (FLOAT_FIELDS.has(key)) return toFloat(v);
  if (BOOL_FIELDS.has(key)) return v === 'yes' ? true : v === 'no' ? false : v;
  if (BIT_FIELDS.has(key)) return v === '1' ? true : v === '0' ? false : v;
  return v;
}

const ENTRY_RE = /<div class='cl-entry([^']*)'>([\s\S]*?)(?=<div class='cl-entry|$)/g;
const COMMENT_RE = /<!--\s*([a-z_][a-z0-9_]*)=([\s\S]*?)-->/g;
const HREF_RE = /<a\s+href='([^']+)'\s+target='_blank'>/g;

/**
 * @param {string} html Raw directory page.
 * @returns {{receivers: object[], skipped: number}}
 */
export function parseDirectory(html) {
  const receivers = [];
  let skipped = 0;

  for (const [, classes, body] of html.matchAll(ENTRY_RE)) {
    const rx = {};

    for (const [, key, value] of body.matchAll(COMMENT_RE)) {
      rx[key] = coerce(key, value);
    }

    // An entry with no id is not something we can key on downstream, and has
    // never appeared in practice. Count it rather than emitting a junk record.
    if (!rx.id) { skipped++; continue; }

    // Both anchors point at the receiver; the avatar link comes first. Taking
    // the first only once the rest agree means a future layout change that adds
    // an unrelated link cannot silently swap the URL — it drops the entry
    // instead, visibly, in the skipped count. Every entry in the directory
    // carries at least two anchors and they have always agreed.
    const hrefs = [...body.matchAll(HREF_RE)].map((m) => decodeEntities(m[1]));
    if (hrefs.length && !hrefs.every((h) => h === hrefs[0])) { skipped++; continue; }
    if (hrefs.length) rx.url = hrefs[0];

    const seq = classes.match(/seq_(\d+)/);
    if (seq) rx.seq = Number(seq[1]);
    const snrAll = classes.match(/snr_all_(-?\d+)/);
    if (snrAll) rx.snr_all = Number(snrAll[1]);
    const snrHf = classes.match(/snr_hf_(-?\d+)/);
    if (snrHf) rx.snr_hf = Number(snrHf[1]);

    // The directory paints these rows red. It is the operator's own signal that
    // something is wrong with the entry, so it is worth carrying through.
    rx.flagged = /\bcl-err-entry\b/.test(classes);

    receivers.push(rx);
  }

  return { receivers, skipped };
}
