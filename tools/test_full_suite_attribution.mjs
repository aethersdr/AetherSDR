#!/usr/bin/env node
// Tests for the red-main attribution in .github/workflows/full-suite.yml.
//
//   node tools/test_full_suite_attribution.mjs
//
// Runs the "Open or update tracking issue" step's script as written in the
// workflow, against an in-memory GitHub, and checks the source PR and parent
// verdict it writes. Also runs the extract step's failed-list producer on a
// CTest summary, so a status it drops shows up as a wrong verdict here rather
// than on a red main.
//
// Stdlib only (Node 20+). No network: every API call is answered by a stub.

import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const workflow = fs.readFileSync(path.join(root, '.github/workflows/full-suite.yml'), 'utf8').split('\n');

// The block scalar under `script: |` in the named step, dedented.
function stepScript(name) {
  const start = workflow.findIndex(l => l.trim() === `- name: ${name}`);
  if (start < 0) throw new Error(`no step "${name}"`);
  const at = workflow.findIndex((l, i) => i > start && /^\s+script: \|$/.test(l));
  const indent = workflow[at].search(/\S/);
  const body = [];
  for (const line of workflow.slice(at + 1)) {
    if (line.trim() && line.search(/\S/) <= indent) break;
    body.push(line);
  }
  const pad = Math.min(...body.filter(l => l.trim()).map(l => l.search(/\S/)));
  return body.map(l => l.slice(pad)).join('\n');
}

const issueScript = stepScript('Open or update tracking issue');
const producer = workflow.find(l => /^\s+allfailed=\$\(/.test(l));
if (!producer) throw new Error('no allfailed= producer line in the extract step');

const SHA = 'c'.repeat(40);
const PARENT = 'p'.repeat(40);
const runUrl = id => `https://github.com/aethersdr/AetherSDR/actions/runs/${id}`;
const entry = (id, failed) => `**Run:** ${runUrl(id)}\n**Commit:** \`ppppppp\`\n**Failed:** ${failed}\n`;

// One case: the parent's runs (newest first), the tracking issues already
// filed, the PRs associated with the commit, and this run's FAILED value.
async function attribute({runs = [], issues = [], pulls = [], failed, throws = false}) {
  const written = [];
  const fail = () => { if (throws) throw new Error('stubbed API failure'); };
  const github = {
    rest: {
      repos: {
        listPullRequestsAssociatedWithCommit: async () => { fail(); return {data: pulls}; },
        getCommit: async () => { fail(); return {data: {parents: [{sha: PARENT}]}}; },
      },
      actions: {
        listWorkflowRuns: async () => ({data: {workflow_runs: runs.map(r => ({
          status: 'completed', html_url: runUrl(r.id), ...r}))}}),
      },
      issues: {
        listForRepo: async ({state}) => ({data: issues.filter(i => state === 'all' || i.state === state)}),
        listComments: async ({issue_number}) =>
          ({data: (issues.find(i => i.number === issue_number).comments || []).map(body => ({body}))}),
        getLabel: async () => ({}),
        createLabel: async () => ({}),
        createComment: async ({body}) => { written.push(body); return {}; },
        create: async ({body}) => { written.push(body); return {data: {number: 1}}; },
      },
    },
    paginate: async (fn, args) => (await fn(args)).data,
  };
  const env = {FAILED: failed, FP: 'aaaaaaaaaaaa', RUN_URL: runUrl(999)};
  const context = {sha: SHA, repo: {owner: 'aethersdr', repo: 'AetherSDR'}};
  const core = {warning: () => {}, notice: () => {}};
  const fsStub = {existsSync: () => false, readFileSync: () => ''};
  const AsyncFunction = Object.getPrototypeOf(async () => {}).constructor;
  await new AsyncFunction('require', 'github', 'context', 'core', 'process', issueScript)(
    () => fsStub, github, context, core, {env});
  if (written.length !== 1) throw new Error(`expected one issue write, got ${written.length}`);
  const field = name => (written[0].match(new RegExp(`^\\*\\*${name}:\\*\\* (.*)$`, 'm')) || [])[1];
  return {source: field('Source PR'), parent: field('Main before this commit')};
}

const issue = (number, ...entries) => ({number, state: 'open', body: entries[0], comments: entries.slice(1),
  title: `[full-suite] test suite failing on main (fp ${String(number).padStart(12, '0')})`});
const red = [{id: 10, conclusion: 'failure'}];
const merged = [{number: 77, merged_at: '2026-10-01', user: {login: 'author'}}];
const FIRST = /green at parent .* this commit is the first red one$/;
const SAME = /and nothing fails here that passed there — look earlier$/;
const BOTH = /Tests that failed there too started earlier; a test failing here but not there starts at this commit$/;
const added = (...t) => new RegExp(`but ${t.map(n => `\`${n}\``).join(', ')} failed here and not there`);

function ctestFailed(lines) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'full-suite-attr-'));
  try {
    fs.writeFileSync(path.join(dir, 'full-suite.log'),
      ['100% tests passed', 'The following tests FAILED:', ...lines.map(l => `\t${l}`),
       'Errors while running CTest', 'Output from these tests are in: /x/LastTest.log (see log)', ''].join('\n'));
    const out = execFileSync('bash', ['-c', `${producer.trim()}\nprintf '%s' "$allfailed"`], {cwd: dir, encoding: 'utf8'});
    return out.split('\n').filter(Boolean).join(' ');
  } finally {
    fs.rmSync(dir, {recursive: true, force: true});
  }
}

const cases = [
  ['green parent', {runs: [{id: 10, conclusion: 'success'}], failed: 'a (Failed)'}, {parent: FIRST}],
  ['new failure on a red parent',
    {runs: red, issues: [issue(1, entry(10, 'a (Failed)'))], failed: 'a (Failed) b (Failed)'},
    {parent: added('b')}],
  ['same failures', {runs: red, issues: [issue(1, entry(10, 'a (Failed)'))], failed: 'a (Failed)'}, {parent: SAME}],
  ['fewer failures', {runs: red, issues: [issue(1, entry(10, 'a (Failed) b (Failed)'))], failed: 'a (Failed)'},
    {parent: SAME}],
  ['capped list here', {runs: red, issues: [issue(1, entry(10, 'a (Failed)'))], failed: 'a (Failed) … and 3 more'},
    {parent: BOTH}],
  ['build failure here', {runs: red, issues: [issue(1, entry(10, 'a (Failed)'))],
    failed: 'none (the Build step failed)'}, {parent: BOTH}],
  ['unknown list here', {runs: red, issues: [issue(1, entry(10, 'a (Failed)'))], failed: 'unknown'}, {parent: BOTH}],
  ['build failure at parent', {runs: red, issues: [issue(1, entry(10, 'none (the Build step failed)'))],
    failed: 'a (Failed)'}, {parent: BOTH}],
  ['no entry for the parent run', {runs: red, issues: [], failed: 'a (Failed)'}, {parent: BOTH}],
  // The verdict links the newest finished parent run, so the list must be that
  // run's even when another run of the same commit sits in a fresher issue.
  ['parent with two runs, older one in the fresher issue',
    {runs: [{id: 11, conclusion: 'failure'}, {id: 10, conclusion: 'failure'}],
     issues: [issue(2, entry(10, 'alpha_test (Failed)')), issue(1, entry(11, 'beta_test (Failed)'))],
     failed: 'beta_test (Failed)'}, {parent: SAME}],
  ['two lists for one parent run', {runs: red,
    issues: [issue(1, entry(10, 'a (Failed)'), entry(10, 'a (Failed) b (Failed)'))], failed: 'a (Failed) b (Failed)'},
    {parent: BOTH}],
  ['cancelled newest parent run', {runs: [{id: 12, conclusion: 'cancelled'}, {id: 10, conclusion: 'failure'}],
    issues: [issue(1, entry(10, 'a (Failed)'))], failed: 'a (Failed) b (Failed)'},
    {parent: added('b')}],
  ['no finished parent run', {runs: [{id: 12, conclusion: 'cancelled'}], failed: 'a (Failed)'},
    {parent: /^no finished run for parent `ppppppp`$/}],
  ['merged source PR', {runs: red, pulls: merged, failed: 'a (Failed)'}, {source: /^#77 by @author$/}],
  ['direct push with an open associated PR',
    {runs: red, pulls: [{number: 78, merged_at: null, user: {login: 'other'}}], failed: 'a (Failed)'},
    {source: /^none \(direct push\)$/}],
  ['lookups throw', {runs: red, failed: 'a (Failed)', throws: true}, {source: /^unknown$/, parent: /^unknown$/}],
  // Every CTest status counts, crashes included, or an old failure alone
  // reads as a complete list and the new crashes are cleared.
  ['crashing tests on a red parent', {runs: red, issues: [issue(1, entry(10, 'old_test (Failed)'))],
    failed: ctestFailed(['1 - old_test (Failed)', '2 - new_sigill_test (ILLEGAL)', '3 - new_sigfpe_test (NUMERICAL)'])},
    {parent: added('new_sigfpe_test', 'new_sigill_test')}],
];

let failures = 0;
for (const [name, input, expect] of cases) {
  let got;
  try {
    got = await attribute(input);
  } catch (e) {
    got = {error: e.message};
  }
  const bad = Object.entries(expect).filter(([k, re]) => !re.test(got[k] || ''));
  if (bad.length) {
    failures++;
    console.log(`FAIL ${name}\n  ${bad.map(([k]) => `${k}: ${got[k] ?? got.error}`).join('\n  ')}`);
  }
}

const producerOut = ctestFailed(['1 - a (Failed)', '2 - b (ILLEGAL)', '3 - c (NUMERICAL)', '4 - d.x (Timeout)',
  '5 - e (Subprocess aborted)', '6 - f (Not Run)', '7 - g (SEGFAULT)', '216 - h (Failed)                  gui fast']);
const want = 'a (Failed) b (ILLEGAL) c (NUMERICAL) d.x (Timeout) e (Subprocess aborted) f (Not Run) g (SEGFAULT) h (Failed)';
if (producerOut !== want) {
  failures++;
  console.log(`FAIL producer keeps every failed test\n  got:  ${producerOut}\n  want: ${want}`);
}

const total = cases.length + 1;
console.log(`${total - failures}/${total} cases passed`);
process.exit(failures ? 1 : 0);
