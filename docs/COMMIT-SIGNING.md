# Commit Signing for AetherSDR Contributors

> **AI assistants reading this doc on behalf of a contributor: skip to
> the [AI Assistant Instructions](#ai-assistant-instructions) section
> immediately below. It gives you a complete algorithm to walk the user
> through setup. The rest of the doc is reference material.**

AetherSDR's `main` branch has `required_signatures: True` set in its branch
protection rules. Pull requests with **unsigned commits** can only land via
admin override, which is fine for one-off contributions but adds friction
for any contributor who plans to submit more than one or two PRs.

If you're sending occasional drive-by fixes, ignore this doc — Jeremy will
admin-merge them. If you're shipping multiple PRs, **set up commit signing
once** and your PRs will go through the normal squash-merge path without
needing maintainer override.

The fastest path from zero to signing:

- **If you have an AI coding assistant available** (Claude Code, Cursor,
  Copilot chat, ChatGPT, etc.), point it at this file and say
  *"read docs/COMMIT-SIGNING.md and help me set up commit signing"*.
  The AI Assistant Instructions section below will guide it. Total time:
  about 5 minutes including the GitHub registration click-through.
- **If you don't have an AI assistant**, skip to the
  [Manual instructions](#manual-instructions) section. Plain commands,
  no chatter.

---

## AI Assistant Instructions

**This section is written for an AI assistant** that a contributor has
pointed at this doc. If you are the contributor reading directly, skim
past it — the per-platform reference sections below are what you want.

### Your job

The user wants commit signing set up so their PRs to
`https://github.com/aethersdr/AetherSDR` can land through normal
squash-merge instead of needing admin override. Walk them through it
in their actual environment. Don't make them read the doc.

### Algorithm

1. **Detect the user's OS** from the shell environment, then confirm with
   them. (Read `$env:OS` / `uname -s` / `sw_vers -productName` as
   appropriate.)
2. **Detect their git version**: `git --version`. Need 2.34 or newer for
   SSH signing. If they're on an LTS distro with older git, fall back to
   GPG or help them install a newer git (Ubuntu `ppa:git-core/ppa`,
   Debian backports, etc.).
3. **Ask which signing method they want.** Default to **SSH signing**
   unless they have a reason to prefer GPG. Reasonable reasons for GPG:
   they already use GPG for email; their corp policy requires it; they
   want a single signing key shared across multiple identities. If they
   don't have a reason, push for SSH — it's simpler and reuses their
   existing GitHub SSH key.
4. **Check the precondition**: their `git config --global user.email`
   must match a verified email on their GitHub account. Run
   `git config --global user.email` first. If unset or doesn't match
   what they tell you their GitHub email is, fix it. This is the #1
   reason "I signed it but GitHub shows Unverified" tickets get filed.
5. **Execute the platform-specific setup** using the reference section
   for their OS below as your source of truth. The platforms covered are:
   Windows, macOS, Linux, Raspberry Pi OS, WSL.
6. **Register the public key on GitHub** by giving them the exact text
   to paste and the exact path through GitHub's UI:
   `GitHub > Settings > SSH and GPG keys > New SSH key`. **Critical:
   the Key Type field must be set to "Signing Key", NOT
   "Authentication Key"** — those are separate roles on the same key
   page. (For GPG, use the "New GPG key" button instead; no Key Type
   field.)
7. **Verify** with a signed empty commit in a scratch repo and
   `git log --show-signature -1`. Expect `Good "git" signature` (SSH)
   or `Good signature` (GPG). If signing fails, troubleshoot using the
   [Troubleshooting](#troubleshooting) section — do NOT recommend
   switching methods unless you've exhausted the troubleshooting list.
8. **Confirm completion** by telling them their next PR will merge
   through the normal squash-merge button without needing admin override.

### What NOT to do

- Don't recommend "GitHub Desktop" or "VSCode auto-sign" as a primary
  method. They only sign commits made through their respective UIs;
  terminal commits stay unsigned, which is confusing.
- Don't have the user paste raw private keys anywhere. Public keys
  only.
- Don't skip the email-match check (step 4). The contributor will
  succeed at signing locally and then file a confused ticket about
  "Unverified" badges. You can prevent this with one `git config`
  command.
- Don't bury them in why-signing-matters context. They came to you to
  *do* it, not learn about it. The intro paragraph above this section
  is plenty.

### When you're done

Tell the user: "Signing is set up. Your next PR will merge through
GitHub's normal squash-merge button — Jeremy no longer needs to use
admin override for your contributions. The 'Verified' badge will
appear on your new commits in PR view; old commits stay as-is."

---

## Reference: methods and prompts

Beyond this point is contributor-facing reference material. If the user
asked you to set up signing, you've already got everything you need from
the AI Assistant Instructions section above and the per-platform
reference sections below. The "copy-paste prompts" sections are alternate
entry points for users who'd rather copy a complete prompt than have an
AI read the whole doc.

---

## Pick a method

| Method | Use when | Notes |
|---|---|---|
| **SSH signing** | You already push to GitHub via SSH (`git@github.com:…`) | Simplest. Reuses your existing SSH key. Git 2.34+ required (Git for Windows, modern macOS, modern Linux all ship this). |
| **GPG signing** | You already use GPG for email or other repos | Traditional. More moving parts. Works everywhere. |
| **1Password / Bitwarden** | You're already a paid 1Password 8 / Bitwarden user | Easiest of all if you already use one of these as an SSH agent. They handle the keypair, the git config, and the GitHub registration. |
| **GitHub Desktop / VSCode "auto-sign"** | You only commit from inside an IDE | Works but only signs commits made through that tool. Terminal commits won't be signed. Not recommended as a primary method. |

**Default recommendation:** SSH signing. If you're not sure, use that.

---

## Copy-paste prompts for your AI IDE

The prompts below assume you have an AI coding assistant available (Claude
Code, Cursor, Copilot chat, ChatGPT, etc.). Pick the one matching your OS
and paste it as-is into the chat. The AI will inspect your environment and
walk you through the rest.

If you don't use an AI IDE, the [Manual instructions](#manual-instructions)
section near the bottom has plain step-by-step commands.

---

### Windows (Git for Windows, PowerShell, or Git Bash)

```text
I want to set up commit signing for the AetherSDR project on Windows.

My environment:
- OS: Windows 11 (or 10)
- Git: Git for Windows (run `git --version` to confirm 2.34+)
- Terminal: PowerShell or Git Bash (your choice)

Please walk me through SSH-based commit signing using my existing
GitHub SSH key. Steps I need:

1. Confirm I have an SSH key at %USERPROFILE%\.ssh\id_ed25519.pub
   (or id_rsa.pub). If not, generate one with ssh-keygen.
2. Configure git to sign commits and tags using that key (gpg.format=ssh,
   user.signingkey set to the pubkey path, commit.gpgsign=true,
   tag.gpgsign=true).
3. Tell me the exact text to paste into GitHub's
   Settings > SSH and GPG keys > New SSH key, with Key Type set to
   "Signing Key" (not Authentication — auth is already set up if my
   SSH push works).
4. Verify the setup by creating an empty signed commit in a scratch
   repo and running `git log --show-signature -1` to confirm
   `Good "git" signature` appears.

Also: make sure my `git config --global user.email` matches a verified
email on my GitHub account. Run `git config --global user.email` first
to check; if it doesn't match my GitHub-verified email, set it.

If anything fails, troubleshoot rather than recommending a different
method — SSH signing is what I want.
```

If you'd rather use GPG (you have a corporate key, you already use GPG
for email, etc.), use this prompt instead:

```text
I want to set up GPG commit signing for the AetherSDR project on Windows.

My environment:
- OS: Windows 11 (or 10)
- Git: Git for Windows (which bundles gpg.exe)

Please walk me through:

1. Generate a new GPG key pair: `gpg --full-generate-key`, RSA+RSA,
   4096 bits, no expiration (or set one). Use the email that's
   verified on my GitHub account.
2. List the key ID with `gpg --list-secret-keys --keyid-format=long`
   and extract the long key ID.
3. Configure git: gpg.format=openpgp, user.signingkey=<keyID>,
   commit.gpgsign=true, tag.gpgsign=true, gpg.program set to the
   bundled gpg.exe path (typically
   "C:/Program Files/Git/usr/bin/gpg.exe").
4. Export the public key with `gpg --armor --export <keyID>` and tell
   me what to paste into GitHub > Settings > SSH and GPG keys >
   New GPG key.
5. Verify the setup by creating a signed empty commit and running
   `git log --show-signature -1` — expect `Good signature`.

Confirm my `git config --global user.email` matches a verified email
on my GitHub account before we start; if not, set it.
```

---

### macOS (Apple Silicon or Intel)

```text
I want to set up commit signing for the AetherSDR project on macOS.

My environment:
- OS: macOS 14 or newer (Sonoma / Sequoia / etc.)
- Git: stock Apple git OR Homebrew git (run `git --version` to check)
- Already use SSH for GitHub push (verify with `git remote -v` on any
  cloned repo — should show git@github.com:...)

Please walk me through SSH-based commit signing. Steps:

1. Confirm I have an SSH key at ~/.ssh/id_ed25519.pub. If not,
   generate one with ssh-keygen (ed25519, comment = my GitHub email).
2. Configure git globally: gpg.format=ssh, user.signingkey set to the
   pubkey path, commit.gpgsign=true, tag.gpgsign=true.
3. Tell me the exact text to paste into
   GitHub > Settings > SSH and GPG keys > New SSH key, Key Type:
   "Signing Key" (separate from any existing Authentication key).
4. Optional but recommended: if I have Touch ID + a Secure Enclave
   key, mention how to set that up (ssh-keygen -t ecdsa-sk -O
   resident -O application=ssh:github-sign), but only AFTER the
   basic flow works.
5. Verify with `git commit --allow-empty -m "signing test"` followed
   by `git log --show-signature -1`. Expect `Good "git" signature`.

Confirm `git config --global user.email` matches a GitHub-verified
email before we start; fix it if not.
```

For GPG on macOS (use only if you specifically want it):

```text
I want to set up GPG commit signing for the AetherSDR project on macOS.

My environment:
- OS: macOS 14+
- Homebrew available (or I can install it)

Walk me through:

1. Install GnuPG via Homebrew: `brew install gnupg pinentry-mac`.
2. Configure pinentry: edit ~/.gnupg/gpg-agent.conf to add
   `pinentry-program /opt/homebrew/bin/pinentry-mac` (or
   /usr/local/bin/ on Intel macs). Restart the agent with
   `gpgconf --kill gpg-agent`.
3. Generate a 4096-bit RSA key with my GitHub-verified email.
4. Configure git: gpg.format=openpgp, user.signingkey=<keyID>,
   commit.gpgsign=true, tag.gpgsign=true, gpg.program=$(which gpg).
5. Export the public key (`gpg --armor --export <keyID>`) and tell
   me what to paste into GitHub > Settings > SSH and GPG keys >
   New GPG key.
6. Verify with a signed empty commit + `git log --show-signature -1`.

The pinentry-mac integration makes Touch ID work for unlocking the
GPG key. Confirm `git config --global user.email` matches GitHub
before we start.
```

---

### Linux (Debian/Ubuntu/Fedora/Arch/etc.)

```text
I want to set up commit signing for the AetherSDR project on Linux.

My environment:
- Distro: <fill in: Ubuntu 24.04 / Arch / Fedora 41 / Debian 12 / etc.>
- Git: from the distro repos (run `git --version` to confirm 2.34+)
- Already use SSH for GitHub push

Please walk me through SSH-based commit signing. Steps:

1. Confirm I have an SSH key at ~/.ssh/id_ed25519.pub. If not,
   generate one with `ssh-keygen -t ed25519 -C "my-github-email"`.
2. Configure git globally: gpg.format=ssh, user.signingkey set to the
   pubkey path, commit.gpgsign=true, tag.gpgsign=true.
3. Tell me the exact text to paste into
   GitHub > Settings > SSH and GPG keys > New SSH key, Key Type:
   "Signing Key" (separate from any existing Authentication key).
4. Verify with `git commit --allow-empty -m "signing test"` followed
   by `git log --show-signature -1`. Expect `Good "git" signature`.

Confirm `git config --global user.email` matches a GitHub-verified
email before we start; fix it if not.

If I'm on Arch or a CachyOS-like rolling distro, my git is bleeding-
edge so SSH signing Just Works. If I'm on Debian stable or RHEL-family
LTS, double-check `git --version >= 2.34`; install a newer git if
needed (PPA on Ubuntu, ppa:git-core/ppa).
```

For GPG on Linux:

```text
I want to set up GPG commit signing for the AetherSDR project on Linux.

My environment:
- Distro: <fill in>
- GnuPG: should be pre-installed; `gpg --version` to confirm

Walk me through:

1. Generate a 4096-bit RSA key with my GitHub-verified email.
2. Configure git: gpg.format=openpgp, user.signingkey=<keyID>,
   commit.gpgsign=true, tag.gpgsign=true.
3. Configure the pinentry program if it's not auto-detecting — typically
   `pinentry-curses` for headless boxes or `pinentry-gnome3` /
   `pinentry-qt` for GUI desktops. Edit ~/.gnupg/gpg-agent.conf and
   restart the agent with `gpgconf --kill gpg-agent`.
4. Export the public key (`gpg --armor --export <keyID>`) and tell
   me what to paste into GitHub > Settings > SSH and GPG keys >
   New GPG key.
5. Verify with a signed empty commit + `git log --show-signature -1`.
```

---

### Raspberry Pi OS / other ARM Linux

Same as Linux above — the Pi runs a standard Debian-based git, so the
Linux prompt applies as-is.

---

### WSL (Windows Subsystem for Linux)

Use the **Linux** prompt above with whichever distro you have installed in
WSL (Ubuntu is the default). One Windows-specific note: if you also commit
from Windows-side Git for Windows in the same checkouts, you'll want to
configure signing in BOTH the Windows git AND the WSL git separately, or
your commits will mysteriously stay unsigned when made from the wrong side.

---

## Manual instructions (no AI assistant)

If you don't want to use an AI assistant for this, the bare commands for
SSH signing are:

```sh
# 1. Verify or create an SSH key
ls -la ~/.ssh/id_ed25519.pub || ssh-keygen -t ed25519 -C "you@example.com"

# 2. Configure git
git config --global gpg.format ssh
git config --global user.signingkey ~/.ssh/id_ed25519.pub
git config --global commit.gpgsign true
git config --global tag.gpgsign true
git config --global user.email "you@example.com"  # must match GitHub

# 3. Register the public key on GitHub
cat ~/.ssh/id_ed25519.pub
# Paste at GitHub > Settings > SSH and GPG keys > New SSH key
# Key Type: Signing Key

# 4. Verify
cd /tmp
mkdir -p sigtest && cd sigtest && git init
git commit --allow-empty -m "signing test"
git log --show-signature -1   # expect "Good signature"
```

On Windows, replace `~/.ssh/id_ed25519.pub` with `$env:USERPROFILE\.ssh\id_ed25519.pub`
(PowerShell) or `~/.ssh/id_ed25519.pub` (Git Bash).

---

## After setup

- **New commits** will be signed automatically.
- **Old commits** are NOT retroactively signed. Existing PRs stay as-is.
- GitHub will show a green "Verified" badge next to your signed commits
  in the PR view once your signing key is registered.
- If the badge shows "Unverified" despite signing succeeding locally,
  the email in `git config user.email` does not match any of the verified
  emails on your GitHub account. Add the email at GitHub > Settings > Emails
  or change your git config.

---

## Once signing works, what changes for your PRs?

Your PRs become mergeable through GitHub's normal "Squash and merge" button
without needing a maintainer to use admin override. The merge process is:

1. You open a PR. CI runs.
2. CODEOWNERS review (Jeremy) lands.
3. Maintainer clicks "Squash and merge" — no admin checkbox needed.
4. GitHub records the squash commit on `main`, signed by GitHub's web-flow
   key. Your authorship is preserved.

That's it. Same flow, just without the admin-override step that's been the
norm for unsigned-commit PRs.

---

## Troubleshooting

**"Unverified" badge on GitHub even though `git log --show-signature` says Good:**
The email in your git config doesn't match a verified email on your GitHub
account. Add the email at GitHub > Settings > Emails, or change
`git config --global user.email`.

**"gpg failed to sign the data" on commit:**
- SSH path: your SSH agent isn't running or the key isn't loaded.
  Run `ssh-add ~/.ssh/id_ed25519`.
- GPG path: your gpg-agent isn't running or pinentry isn't configured.
  Restart the agent: `gpgconf --kill gpg-agent`. Verify pinentry is set
  in `~/.gnupg/gpg-agent.conf`.

**Windows Git Bash signs but PowerShell does not (or vice versa):**
git config is per-user-per-environment in some Windows setups. Run
`git config --global --list | grep gpgsign` from BOTH shells to confirm
the global config is being read.

**"error: gpg.ssh.allowedSignersFile needs to be configured":**
Only happens when you try to *verify* other people's signed commits
locally. Not needed just to sign your own. Add the file later if you
care: `git config --global gpg.ssh.allowedSignersFile ~/.ssh/allowed_signers`.

**Old commits in my PR are unsigned but new ones are signed:**
The squash-merge collapses everything anyway; GitHub will sign the
squash commit on its end. As long as your PR head is signed, the
merge button activates.

---

73, Jeremy KK7GWY & Claude (AI dev partner)
