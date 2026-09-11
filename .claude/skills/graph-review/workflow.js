export const meta = {
  name: 'graph-review',
  description: 'Adversarial AetherSDR PR review as a graph: parallel audits, single build, bridge drive, per-finding refutation, one synthesis',
  whenToUse: 'Invoked by the /graph-review skill after its inline preflight and gather steps. Not for direct use.',
  phases: [
    { title: 'Audit', detail: 'issue-fit, scope→preference, governance, quality — in parallel with the build' },
    { title: 'Verify', detail: 'two refuters per finding (reproduce, calibrate) as each audit lands' },
    { title: 'Drive', detail: 'one bridge session against the demo backend for every runtime claim' },
    { title: 'Synthesize', detail: 'one review payload + operator report' },
  ],
}

// ---------------------------------------------------------------------------
// args (from the coordinator, see SKILL.md step 2):
//   pr, title, author, body, headSha, baseSha, linkedIssues[], files[], commits[],
//   headWorktree, baseWorktree|null, tmpDir, scratchDir, buildBase (bool),
//   preflight: { socketTestFound, socketTargets[], notice },
//   ciSummary, botComments (string|null), skillDir
// ---------------------------------------------------------------------------

const A = args
if (!A || !A.pr) throw new Error('graph-review: args.pr is required')
const NODES = `${A.skillDir}/nodes`

const SEV = { enum: ['blocker', 'maintainer-decision', 'nit'] }
const FINDING = {
  type: 'object',
  properties: {
    title: { type: 'string' },
    severity: SEV,
    category: { type: 'string' },
    file: { type: 'string' },
    line: { type: 'integer' },
    inDiff: { type: 'boolean' },
    evidence: { type: 'string' },
    ruleCited: { type: 'string' },
    fix: { type: 'string' },
    needsRuntime: { type: 'boolean' },
  },
  required: ['title', 'severity', 'category', 'file', 'line', 'inDiff', 'evidence', 'fix', 'needsRuntime'],
}
const CLAIM = {
  type: 'object',
  properties: { claim: { type: 'string' }, source: { type: 'string' }, howToTest: { type: 'string' } },
  required: ['claim', 'source', 'howToTest'],
}
const SCOPE_ROW = {
  type: 'object',
  properties: { group: { type: 'string' }, changes: { type: 'string' }, claimed: { type: 'string' }, verdict: { type: 'string' } },
  required: ['group', 'changes', 'claimed', 'verdict'],
}
const AUDIT = {
  type: 'object',
  properties: {
    summary: { type: 'string' },
    verdict: { type: 'string' },
    findings: { type: 'array', items: FINDING },
    runtimeClaims: { type: 'array', items: CLAIM },
    attacksSurvived: { type: 'array', items: { type: 'string' } },
    testsToInvert: { type: 'array', items: { type: 'object', properties: { test: { type: 'string' }, mutation: { type: 'string' } }, required: ['test', 'mutation'] } },
    scopeTable: { type: 'array', items: SCOPE_ROW },
    uiRows: { type: 'array', items: SCOPE_ROW },
    checklistHolds: { type: 'string' },
    requirementMap: { type: 'array', items: { type: 'object', properties: { requirement: { type: 'string' }, hunk: { type: 'string' }, addressed: { type: 'boolean' } }, required: ['requirement', 'hunk', 'addressed'] } },
    unexplained: { type: 'array', items: { type: 'string' } },
    classifications: { type: 'array', items: { type: 'object', properties: { change: { type: 'string' }, kind: { enum: ['fix', 'preference'] }, authority: { type: 'string' } }, required: ['change', 'kind', 'authority'] } },
  },
  required: ['summary', 'findings', 'runtimeClaims', 'attacksSurvived'],
}
const VERDICT = {
  type: 'object',
  properties: {
    refuted: { type: 'boolean' },
    reason: { type: 'string' },
    severityAdjusted: { anyOf: [SEV, { type: 'null' }] },
    evidence: { type: 'string' },
  },
  required: ['refuted', 'reason', 'severityAdjusted'],
}
const BUILD = {
  type: 'object',
  properties: {
    ok: { type: 'boolean' },
    buildDir: { type: 'string' },
    baseBuildDir: { type: 'string' },
    targets: { type: 'array', items: { type: 'object', properties: { target: { type: 'string' }, result: { type: 'string' }, output: { type: 'string' } }, required: ['target', 'result'] } },
    inversions: { type: 'array', items: { type: 'object', properties: { test: { type: 'string' }, mutation: { type: 'string' }, noticed: { type: 'boolean' }, output: { type: 'string' } }, required: ['test', 'mutation', 'noticed'] } },
    errorTail: { type: 'string' },
    notes: { type: 'string' },
  },
  required: ['ok', 'buildDir', 'targets', 'inversions'],
}
const DRIVE = {
  type: 'object',
  properties: {
    drove: { type: 'boolean' },
    instance: { type: 'string' },
    reason: { type: 'string' },
    results: { type: 'array', items: { type: 'object', properties: { claim: { type: 'string' }, outcome: { enum: ['confirmed', 'refuted', 'unreachable'] }, evidence: { type: 'string' } }, required: ['claim', 'outcome', 'evidence'] } },
    newFindings: { type: 'array', items: FINDING },
    incidents: { type: 'array', items: { type: 'string' } },
  },
  required: ['drove', 'results', 'newFindings', 'incidents'],
}
const SYNTH = {
  type: 'object',
  properties: {
    review: {
      type: 'object',
      properties: {
        event: { enum: ['REQUEST_CHANGES', 'COMMENT'] },
        body: { type: 'string' },
        comments: { type: 'array', items: { type: 'object', properties: { path: { type: 'string' }, line: { type: 'integer' }, start_line: { type: 'integer' }, side: { type: 'string' }, body: { type: 'string' } }, required: ['path', 'line', 'side', 'body'] } },
      },
      required: ['event', 'body', 'comments'],
    },
    report: { type: 'string' },
    recommendation: { enum: ['Approve', 'Approve with nits', 'Request changes', 'Needs maintainer decision'] },
    socketNotice: { type: 'string' },
  },
  required: ['review', 'report', 'recommendation'],
}

// Shared context block every node prompt starts with.
const CONTEXT = `
AetherSDR PR #${A.pr} — "${A.title}" by @${A.author}
head ${A.headSha}  base ${A.baseSha}
Linked issues: ${A.linkedIssues.length ? A.linkedIssues.map(n => '#' + n).join(', ') : 'none'}
PR-head worktree (READ-ONLY for you): ${A.headWorktree}
${A.baseWorktree ? 'Merge-base worktree (READ-ONLY for you): ' + A.baseWorktree : ''}
Preflight: ${A.preflight.socketTestFound ? 'SOCKET TEST FOUND — targets ' + A.preflight.socketTargets.join(', ') + ' must not be built or run' : 'no socket-owning test in the PR'}
CI: ${A.ciSummary}
Files:
${A.files.join('\n')}
Commits (author date):
${A.commits.join('\n')}

PR body:
---
${A.body}
---
Read ${NODES}/stance.md before doing anything else, then your node file.
`

const audit = (node, extra = '') => agent(
  `${CONTEXT}\nYou are the **${node}** node. Read ${NODES}/${node}.md and do exactly that job.${extra}\nReturn the AUDIT structure. Every finding needs the full finding shape from stance.md.`,
  { label: `audit:${node}`, phase: 'Audit', schema: AUDIT })

// ---------------------------------------------------------------------------
// Phase 1+2: audits fan out; each finding is refuted as soon as its audit lands.
// The build runs concurrently with the audits (it needs none of their output
// except testsToInvert, which the coordinator collected up front from the diff).
// ---------------------------------------------------------------------------
phase('Audit')
log(`PR #${A.pr}: fanning out 4 audits + build`)

const DIMENSIONS = ['issue-fit', 'scope', 'governance', 'quality']

let findingSeq = 0
const tag = (f, origin) => ({ ...f, id: `F${++findingSeq}`, origin })

const verifyOne = async (f) => {
  const lenses = ['reproduce', 'calibrate']
  const votes = await parallel(lenses.map(lens => () => agent(
    `${CONTEXT}\nYou are a **verify** node with the **${lens}** lens. Read ${NODES}/verify.md.\n` +
    `The finding to refute (produced by the ${f.origin} node):\n${JSON.stringify(f, null, 2)}\n` +
    `Default to refuted=true if the evidence is thin. Return the VERDICT structure.`,
    { label: `verify:${lens}:${f.id}`, phase: 'Verify', schema: VERDICT })))
  const vs = votes.filter(Boolean)
  const refuted = vs.some(v => v.refuted)
  const adj = vs.map(v => v.severityAdjusted).find(Boolean)
  return { ...f, refuted, severity: refuted ? f.severity : (adj || f.severity), verifierNotes: vs.map((v, i) => `${lenses[i]}: ${v.reason}${v.evidence ? ' — ' + v.evidence : ''}`) }
}

const auditChain = pipeline(
  DIMENSIONS,
  // stage 1: the audit itself; scope additionally feeds the preference node
  async (dim) => {
    const r = await audit(dim)
    if (!r) return null
    if (dim === 'scope') {
      const pref = await audit('preference',
        `\nThe scope node's uiRows (rows touching existing UI/defaults/behavior):\n${JSON.stringify(r.uiRows || [], null, 2)}\nIts scope table for context:\n${JSON.stringify(r.scopeTable || [], null, 2)}`)
      if (pref) {
        r.findings = [...r.findings, ...pref.findings.map(f => ({ ...f, origin: 'preference' }))]
        r.runtimeClaims = [...r.runtimeClaims, ...pref.runtimeClaims]
        r.attacksSurvived = [...r.attacksSurvived, ...pref.attacksSurvived]
        r.classifications = pref.classifications || []
      }
    }
    return r
  },
  // stage 2: verify each finding of this dimension, no barrier on the other dimensions
  async (r, dim) => {
    if (!r) return null
    const tagged = r.findings.map(f => tag(f, f.origin || dim))
    const verified = await parallel(tagged.map(f => () => verifyOne(f)))
    return { dim, audit: r, findings: verified.filter(Boolean) }
  },
)

const buildTask = agent(
  `${CONTEXT}\nYou are the **build** node. Read ${NODES}/build.md.\n` +
  `TMPDIR to export: ${A.tmpDir}\n` +
  `Build the merge base too: ${A.buildBase ? 'YES (' + A.baseWorktree + ')' : 'no'}\n` +
  `Tests to invert (from the coordinator's read of the diff): ${JSON.stringify(A.testsToInvert || [])}\n` +
  `Return the BUILD structure.`,
  { label: 'build', phase: 'Audit', schema: BUILD })

const [auditResults, build] = await Promise.all([auditChain, buildTask.catch(() => null)])
const audits = auditResults.filter(Boolean)

const allFindings = audits.flatMap(a => a.findings)
const confirmed = allFindings.filter(f => !f.refuted)
const refuted = allFindings.filter(f => f.refuted)
log(`audits done: ${allFindings.length} findings, ${confirmed.length} survived refutation, ${refuted.length} refuted; build ${build && build.ok ? 'ok' : 'FAILED'}`)

// Inversions the audits asked for that the coordinator did not already pass.
const wantedInversions = audits.flatMap(a => a.audit.testsToInvert || [])
const doneInversions = new Set((build && build.inversions || []).map(i => i.test + '|' + i.mutation))
const missedInversions = wantedInversions.filter(i => !doneInversions.has(i.test + '|' + i.mutation))
if (missedInversions.length) log(`NOTE: ${missedInversions.length} inversion request(s) from audits were not run (build ran concurrently); listed in the report`)

// ---------------------------------------------------------------------------
// Phase 3: one drive session. Needs ALL runtime claims + the build → genuine barrier.
// ---------------------------------------------------------------------------
phase('Drive')
const runtimeClaims = [
  ...audits.flatMap(a => a.audit.runtimeClaims || []),
  ...confirmed.filter(f => f.needsRuntime).map(f => ({ claim: `[${f.id}] ${f.title}`, source: f.origin, howToTest: f.evidence })),
]
const dedupClaims = []
const seenClaims = new Set()
for (const c of runtimeClaims) { const k = c.claim.trim().toLowerCase(); if (!seenClaims.has(k)) { seenClaims.add(k); dedupClaims.push(c) } }

let drive = null
if (build && build.ok && dedupClaims.length) {
  drive = await agent(
    `${CONTEXT}\nYou are the **drive** node. Read ${NODES}/drive.md.\n` +
    `Head build dir: ${build.buildDir}\n${build.baseBuildDir ? 'Merge-base build dir: ' + build.baseBuildDir + '\n' : ''}` +
    `Scratch dir: ${A.scratchDir}\n` +
    `Runtime claims to settle (${dedupClaims.length}):\n${JSON.stringify(dedupClaims, null, 2)}\n` +
    `Return the DRIVE structure.`,
    { label: 'drive', phase: 'Drive', schema: DRIVE })
} else {
  log(build && build.ok ? 'no runtime claims to drive' : 'build failed — drive skipped, all runtime claims stay unreachable')
}

// Findings that needed runtime evidence and did not get it are downgraded to "reasoned-from-code".
const driveOutcome = new Map((drive && drive.results || []).map(r => [r.claim.trim().toLowerCase(), r]))
for (const f of confirmed) {
  if (!f.needsRuntime) continue
  const r = driveOutcome.get(`[${f.id}] ${f.title}`.trim().toLowerCase())
  if (!r || r.outcome === 'unreachable') { f.runtime = 'unverified'; continue }
  f.runtime = r.outcome; f.driveEvidence = r.evidence
  if (r.outcome === 'refuted') { f.refuted = true; f.verifierNotes.push(`drive: refuted — ${r.evidence}`) }
}
const finalConfirmed = confirmed.filter(f => !f.refuted)
const driveRefuted = confirmed.filter(f => f.refuted)
const newFromDrive = (drive && drive.newFindings || []).map(f => tag(f, 'drive'))

// ---------------------------------------------------------------------------
// Phase 4: synthesis.
// ---------------------------------------------------------------------------
phase('Synthesize')
const scopeAudit = audits.find(a => a.dim === 'scope')
const issueAudit = audits.find(a => a.dim === 'issue-fit')
const synth = await agent(
  `${CONTEXT}\nYou are the **synthesize** node. Read ${NODES}/synthesize.md.\n` +
  `Preflight notice (include verbatim if non-empty): ${A.preflight.notice || ''}\n\n` +
  `Issue-fit verdict: ${issueAudit ? issueAudit.audit.verdict : 'n/a'}\n${issueAudit ? issueAudit.audit.summary : ''}\n` +
  `Requirement map:\n${JSON.stringify(issueAudit && issueAudit.audit.requirementMap || [], null, 2)}\n\n` +
  `Scope table:\n${JSON.stringify(scopeAudit && scopeAudit.audit.scopeTable || [], null, 2)}\n` +
  `Body checklist holds: ${scopeAudit && scopeAudit.audit.checklistHolds || 'not assessed'}\n` +
  `Preference classifications:\n${JSON.stringify(scopeAudit && scopeAudit.audit.classifications || [], null, 2)}\n\n` +
  `CONFIRMED findings (${finalConfirmed.length + newFromDrive.length}):\n${JSON.stringify([...finalConfirmed, ...newFromDrive], null, 2)}\n\n` +
  `REFUTED findings — do NOT report as findings; summarize one line each under "What I tried to break" (${refuted.length + driveRefuted.length}):\n${JSON.stringify([...refuted, ...driveRefuted].map(f => ({ id: f.id, title: f.title, origin: f.origin, verifierNotes: f.verifierNotes })), null, 2)}\n\n` +
  `Attacks survived (union):\n${JSON.stringify(audits.flatMap(a => a.audit.attacksSurvived || []), null, 2)}\n\n` +
  `Build:\n${JSON.stringify(build, null, 2)}\n` +
  `Inversions requested but not run: ${JSON.stringify(missedInversions)}\n\n` +
  `Drive:\n${JSON.stringify(drive, null, 2)}\n\n` +
  `Return the SYNTH structure. The review payload is posted verbatim by the coordinator.`,
  { label: 'synthesize', phase: 'Synthesize', schema: SYNTH })

return {
  synth,
  stats: {
    findingsRaised: allFindings.length + newFromDrive.length,
    confirmed: finalConfirmed.length + newFromDrive.length,
    refuted: refuted.length + driveRefuted.length,
    runtimeClaims: dedupClaims.length,
    drove: !!(drive && drive.drove),
    buildOk: !!(build && build.ok),
    missedInversions,
  },
  headWorktree: A.headWorktree,
  baseWorktree: A.baseWorktree,
}
