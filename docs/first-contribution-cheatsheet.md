# Your First AetherSDR Contribution — the cheat sheet

Companion to the video tutorial *(link will be added when the video is
published)*. The video uses Claude Code, but the same asks work with GitHub
Copilot or OpenAI Codex. Everything here is a sentence you say to your AI
partner — you never type commands. For the full contribution rules, see
[CONTRIBUTING.md](../CONTRIBUTING.md); this page is the beginner's on-ramp.
(Curious what the AI actually ran? Bottom of the page.)

## What you need

1. **VS Code** (free) — [code.visualstudio.com](https://code.visualstudio.com)
2. **One AI coding agent — choose any one; these are separate alternatives:**
   - **GitHub Copilot Free:** follow the
     [Copilot quickstart](https://docs.github.com/en/copilot/get-started/quickstart?tool=vscode)
     and sign in to VS Code with your GitHub account.
   - **OpenAI Codex:** install the
     [Codex extension](https://developers.openai.com/codex/ide) and sign in
     with your ChatGPT account. The Free plan works in VS Code with lower
     usage limits.
   - **Claude Code:** install the
     [Claude Code extension](https://code.claude.com/docs/en/vs-code) and use
     a Claude plan that includes Claude Code. The free tier is web-chat only
     and cannot drive VS Code.
3. **A GitHub account** — free; step-by-step below

### Start free with GitHub Copilot

Copilot Free is the simplest no-cost starting point because it uses the GitHub
account you already need. It includes local **Agent mode** in VS Code, so it
can read files, edit code, and run commands. The free allowance is deliberately
limited — a monthly cap on code completions, and a separate, smaller monthly
cap on chat and agent requests. The second one is what this page runs on: every
ask below is an agent request, so a long build-debug-test session is what
reaches the limit, not typing. That is still enough to learn the workflow or
try a small contribution. The free plan also does not include the cloud coding
agent or pull-request reviews.

Current limits:
**[github.com/features/copilot/plans](https://github.com/features/copilot/plans)**.
If you reach the free allowance, wait for it to reset or choose a paid plan;
you do not need to switch agents unless you want to.

### Which Claude plan?

| Plan | Runs the agent? | What fits |
|---|---|---|
| Free | ❌ | Window-shopping — chat about the project in a browser |
| Pro (entry paid tier) | ✅ | **Start here.** A real bug fix per sitting — explore, fix, test, submit |
| Max (top tier) | ✅ | The ceiling disappears — all-day sessions, bigger features |

Current prices: **[claude.com/pricing](https://claude.com/pricing)**. If you
hit a usage limit mid-session: nothing is lost, it resets the same day — and
hitting it regularly is the upgrade signal, not a failure.

### Which Codex plan?

Codex is included across ChatGPT plans, including Free and Go, and you can use
it through the VS Code extension. Usage limits vary by plan.

| Plan | What fits |
|---|---|
| Free | Try Codex in VS Code on a quick task or small documentation change |
| Go | Lightweight coding tasks |
| Plus | **Start here.** A few focused contribution sessions each week |
| Pro | Higher limits for frequent or all-day work |

Current plan details:
**[developers.openai.com/codex/pricing](https://developers.openai.com/codex/pricing)**.
The asks below work unchanged with any one of the three agents. Copilot reads
[`.github/copilot-instructions.md`](../.github/copilot-instructions.md), Codex
reads the canonical [AGENTS.md](../AGENTS.md) directly, and Claude Code reads
[CLAUDE.md](../CLAUDE.md). Both tool-specific files point to `AGENTS.md`, so
every option gets the same project rules.

### Creating your GitHub account

GitHub is where AetherSDR lives, and this account is the one thing your AI
partner can't create for you. It's free.

1. Go to [github.com/signup](https://github.com/signup) and pick a username —
   it's public, so **your callsign makes a great one**.
2. Use an email you actually check, and click the verification link GitHub
   sends you.
3. Turn on **two-factor authentication** (Settings → Password and
   authentication). Any authenticator app works. Print the recovery codes it
   shows you, file them with your license paperwork, and **never paste them
   anywhere — including into Claude**.
4. Recommended: Settings → Emails → check **"Keep my email addresses
   private"** so your address stays out of your public activity.

That's it — you never need to learn the rest of the GitHub website. Your
agent drives it from here.

## The four rules of working with your agent

1. It works in the folder you have open — that's its bench space.
2. It asks permission before acting — you're always the control operator.
3. It sounds confident even when wrong — **read what it proposes before you
   say yes.**
4. Never paste passwords, private keys, or two-factor codes into the chat.

## The asks, in order

Copy, paste, replace anything in [brackets].

1. `Hello! I'm brand new to this. What can you do?`
2. `I want to contribute to the AetherSDR project at github.com/aethersdr/AetherSDR. My GitHub username is [YOURS]. Set up everything I need on this computer: install git and the GitHub command-line tool, sign me in, make my own copy of the project on GitHub, and download it into this folder.`
   *(This installs **git** — the logbook tool — and the **GitHub CLI**
   (`gh`), which is how your agent talks to GitHub on your behalf: signing
   you in, creating your fork, and later commenting on issues and opening
   pull requests for you. The sign-in shows a short code you type into your
   browser — that's you authorizing it; the agent never sees your password.
   If the agent ever seems unable to open an issue or pull request, say:
   `Also install the GitHub command-line tool "gh" and sign me in with it.`)*
3. `Before we change anything — what are this project's most important rules? Summarize them for a beginner.`
4. `Read docs/COMMIT-SIGNING.md and help me set up commit signing.`
   *(One thing only you can do: paste the key it gives you into GitHub →
   Settings → SSH and GPG keys → New SSH key, and set **Key type: Signing
   Key** — not Authentication Key.)*
5. `Install everything this project needs, build it, and run it so I can see it.` *(Documentation-only contribution? Skip this one.)*
6. `Show me this project's open issues labeled "good first issue" and explain the top few in plain English. Which one would you recommend for my very first contribution, and why?`
7. `Check whether issue #[N] is already claimed — is anyone assigned to it? If it's free, or only the triage bot is on it, assign me to claim it. If someone else has it, help me pick a different one.`
   *(Claiming is how this project stops two people fixing the same bug. The
   claim is the **assignees list** on the issue, not a comment. The triage bot
   `@aethersdr-agent` is auto-assigned to everything, so seeing only that one
   means the issue is free — add yourself alongside it.)*
8. `Let's work on issue #[N]. Make a branch for it, then explain the bug to me like I'm brand new.`
9. `Show me exactly what you changed and walk me through it, line by line, in plain English.`
10. `Are you sure you caught every case the issue mentions? Double-check before we go further.`
11. `Rebuild and run it. Then tell me exactly how to reproduce the original bug, step by step, so I can check it's really fixed.`
12. `Now prove it. Read docs/automation-bridge.md and use the automation bridge to test this change — show me the before-and-after evidence.` *(See [Prove it with the automation bridge](#prove-it-with-the-automation-bridge) below.)*
13. `Write the commit for this fix following the project's conventions, and show me the message before you make it.`
14. `Now push it to my fork and open a pull request against the project. Fill in their template, include the bridge evidence and screenshots, and show me everything before you submit.`
15. *(When a reviewer comments)* `A reviewer asked for a change — here's their comment. Make the change, and update the pull request.`
16. *(After the merge)* `The pull request was merged! Tidy up — sync my copy with the project and clean up the branch. Then: what should we look at next?`

## Prove it with the automation bridge

"It looks right on my screen" is not evidence. AetherSDR ships an **agent
automation bridge**: a switch you flip at launch that lets your agent drive the
running app directly — read the state of any control, click buttons, move
sliders, and screenshot the panadapter. Your agent does all of it; you read the
results.

Think of it as putting the radio on a service monitor instead of eyeballing the
S-meter. Reviewers here look for that trace, and a first PR that arrives with
one gets merged faster than one that says "works for me."

The full reference is [docs/automation-bridge.md](automation-bridge.md) — it is
written *for your agent*, so you do not have to read it. Just say the word
"bridge" and point it there.

### The asks that produce evidence

1. `Read docs/automation-bridge.md, then launch AetherSDR with the automation bridge enabled and confirm you can talk to it.`
2. `Using the bridge, capture the state of the controls this issue touches — before my fix. Save it as the "before" evidence.`
3. `Now with my fix in, capture the same state again and show me a side-by-side. Did anything change that shouldn't have?`
4. `Grab a screenshot of the panadapter (or the dialog I changed) through the bridge so I can put it in the pull request.`
5. `Drive the exact steps from the issue's reproduction through the bridge, and assert the bug no longer happens.`
6. `Is there a bridge verb that would have caught this bug automatically? If the project is missing one, say so in the pull request.`

### Ground rules

- **Check the radio is free first.** A bridge launch reconnects to your last
  radio exactly as a normal launch does, so it will happily join a session
  someone else is already using — and two clients fighting over one radio
  produce evidence that is simply wrong. Ask:
  `Before launching: is AetherSDR already running, and is the radio in use? If
  so, start this test run idle instead of auto-connecting, and don't touch the
  other session.` Your agent can start idle by clearing the
  `AutoConnectToLastRadio` setting, then pick a radio deliberately with the
  bridge's `connect` verb.
- **Transmit is gated on purpose.** The bridge refuses to key the radio unless
  transmit is explicitly enabled, and even then it belongs into a dummy load.
  As a first-timer, say: `Receive-only testing please — do not enable transmit.`
- **Assert on state, not on pixels.** A screenshot is for humans in the PR;
  the real proof is the agent reading the control's actual value and comparing
  it. Ask for both.
- **Evidence goes in the pull request.** Paste the before/after and the
  screenshots into the PR body. That is what turns "I think I fixed it" into a
  reviewable claim.

If any of this fails, you already know the sentence: paste the error and ask
what it means.

## When anything goes wrong

Copy whatever you see — error text, red check, confusing message — paste it to
your agent, and add:

> **"This happened. What does it mean and what should we do?"**

That sentence is the entire troubleshooting manual. Two specials worth
memorizing: `GitHub shows my commit as Unverified.` (signing email mismatch —
it knows) and `Keep this change to what the issue actually asks. Undo the
rest.` (scope control).

## Words you'll hear

| Word | Plain meaning |
|---|---|
| repository / repo | A project's public locker on GitHub |
| fork | Your own copy of the project, to experiment on |
| clone | Downloading your copy to your computer |
| git / commit | The project's logbook / one logged entry of changes |
| branch | A scratch copy of the logbook for one job |
| diff | The before-and-after view — red out, green in |
| pull request (PR) | Your formal ask: "please take my change" — traffic to net control |
| CI | Robots that build and test your change on every OS before a human looks |
| merge | A maintainer accepts your change into the project — you can't press it, and that's the safety net |

<details>
<summary><b>If you want to know what the agent actually ran</b> (you never need this)</summary>

Dependencies (Linux/Debian-family shown; Arch, Fedora, and macOS Homebrew
equivalents are in the [project README](../README.md#building-from-source)):

```bash
sudo apt install qt6-base-dev qt6-base-private-dev qt6-multimedia-dev \
  qt6-websockets-dev qt6-serialport-dev qt6-shader-baker qt6-shadertools-dev \
  cmake ninja-build pkg-config autoconf automake libtool \
  libfftw3-dev portaudio19-dev libhidapi-dev qtkeychain-qt6-dev \
  libxkbcommon-dev libopengl0 gstreamer1.0-pulseaudio gstreamer1.0-plugins-base
```

Build and run (from inside the clone the fork step below created):

```bash
cd AetherSDR
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(nproc)
./build/AetherSDR
```

Windows adds: the VS 2022 MSVC environment (`vcvars64.bat`), two setup scripts
(`scripts\setup\setup-fftw.ps1`, `scripts\setup\setup-qtkeychain.ps1`), and
`-DCMAKE_PREFIX_PATH` pointing at your Qt kit — see the
[Windows 11 section of the README](../README.md#windows-11).

GitHub connection (sign-in, fork, and how the agent opens issues and PRs).
This is the step that creates the `AetherSDR` folder the build block above
runs in:

```bash
gh auth login                                 # device-code sign-in via your browser
gh repo fork aethersdr/AetherSDR --clone      # your fork + local download
# later, on your behalf:
gh issue view <N> --json assignees            # is it already claimed?
gh issue edit <N> --add-assignee @me          # claiming an issue
gh pr create --fill                           # opening the pull request
```

Commit signing (SSH path). Note the sandbox: the verification commit is made
in a throwaway repo under `/tmp`, so a stray empty commit never lands on your
working branch and ships in your first PR:

```bash
# 1. Key
ls ~/.ssh/id_ed25519.pub || ssh-keygen -t ed25519 -C "you@example.com"

# 2. Configure git
git config --global gpg.format ssh
git config --global user.signingkey ~/.ssh/id_ed25519.pub
git config --global commit.gpgsign true
git config --global tag.gpgsign true
git config --global user.email "you@example.com"   # must match GitHub

# 3. Register the PUBLIC key on GitHub — without this, commits sign locally
#    but GitHub shows "Unverified" and branch protection blocks the merge.
cat ~/.ssh/id_ed25519.pub
#    Paste at GitHub > Settings > SSH and GPG keys > New SSH key
#    Key type: Signing Key   (NOT Authentication Key)

# 4. Teach git to verify your own SSH signatures, or step 5 reports
#    "No signature" on a commit that is in fact correctly signed.
echo "you@example.com $(cat ~/.ssh/id_ed25519.pub)" >> ~/.ssh/allowed_signers
git config --global gpg.ssh.allowedSignersFile ~/.ssh/allowed_signers

# 5. Verify, in a throwaway repo
cd /tmp && mkdir -p sigtest && cd sigtest && git init
git commit --allow-empty -m "signing test"
git log --show-signature -1        # expect: Good "git" signature
```

Full details: [docs/COMMIT-SIGNING.md](COMMIT-SIGNING.md) and
[CONTRIBUTING.md](../CONTRIBUTING.md).

</details>

---

*AetherSDR is free, open-source (GPL v3), built by hams:
[github.com/aethersdr/AetherSDR](https://github.com/aethersdr/AetherSDR) —
"Ham radio has a long tradition of helping each other learn — bring that
spirit here." 73*
