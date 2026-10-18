# GitHub release preparation

## Current status

The repository is local and committed. No Git remote exists. No GitHub
repository was created, no push was performed, and no hosted CI run exists.
The connected GitHub account identified itself as `MvsArjun`; this is a
read-only account check, not publication authorization or proof of Git push
authentication. GitHub CLI (`gh`) is not installed on the current PATH.

The user explicitly chose **Keep MIT** during this audit. The existing license
was retained. Existing Git history was preserved; no reset, rebase, force push,
commit deletion or history rewriting was performed.

## Material to review

* [Audit report](audit-report.md): defects, actual results, source changes,
  benchmark findings, and unresolved verification.
* [Release manifest](release-manifest.md): files changed from the pre-audit
  commit, and the staged/committed release contents.
* [Verification](verification.md): Clang success and the failed GCC runs.
* [GCC dependency issue](toolchain-issue.md): an unchanged mandatory failing
  regression and engine-free reproducer; this blocker is not hidden.
* [Scan report](../results/audit-verification/release-scan.json): current tree,
  known credential patterns, sensitive filenames, size/artifact checks, history.

The current publishable tree has personal absolute paths removed from metadata
and logs. Original history still contains **11 historical blobs with personal
path strings** from the initial build. The scan found no matching credential
patterns in those blobs. A pattern scan is not a proof that every secret is
absent. The working tree and an exported source archive can be reviewed without
publishing history; the audit did not rewrite that history without permission.

The GitHub account name above is intentional public destination context, not a
credential. Build trees, compiler installations, temporary sources, executable
archives and local profiling traces are ignored or kept outside the repository.
Small raw benchmark observations are intentional reproducibility evidence.

## Required decisions before publication

The user must confirm the repository name, visibility (public/private),
destination account, and explicit approval to publish the reviewed contents.
They should also decide whether to publish the existing history with its old
personal path strings, or authorize a separate history-cleanup process.
That cleanup is not implied by approval to edit current source files.

The GCC runtime failure and unavailable Linux CI mean the audit does not
recommend describing the release as fully verified. Before a public portfolio
release, verify a corrected runtime with the supplied reproducer and run actual
Linux CI. A private review repository could retain these as documented open issues
if explicitly approved.

## Authentication and later publication procedure

If using GitHub CLI, install it from the official distribution and then
authenticate interactively. The CLI's browser login and Git credential setup
are documented in the [official authentication manual](https://cli.github.com/manual/gh_auth_login).
Never paste a token into a committed file or a command log.

```sh
gh auth login --web --git-protocol https
gh auth setup-git
gh auth status
```

After all required approvals, use the approved account/name and exactly one
approved visibility. The [official repository-creation manual](https://cli.github.com/manual/gh_repo_create)
documents creating from a local source and pushing. These are instructions for
the later approved action; none were executed during the audit.

Before pushing, show `git status --short`, `git diff --stat bf0e409..HEAD`,
`git log --oneline bf0e409..HEAD`, and the release manifest. Re-run the scanner,
confirm the remote URL, then execute only the approved publication operation.
After pushing, inspect the actual remote files and Actions runs. A workflow
file or successful actionlint run does not establish successful hosted CI.
