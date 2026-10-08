# Contributing to RebellioCap

Useful contributions include reproducible bug reports, documentation corrections,
focused tests and improvements to capture, audio, editing or the desktop interface.
Start with [Getting started](docs/getting-started.md) and the
[architecture overview](docs/architecture.md) to understand the project.

## Before starting

Search [existing issues](https://github.com/kirzep/RebellioCap/issues) before
opening a new one. Describe the problem and intended behavior before investing in
a large change, a new dependency or a change to the recording/settings contract.
Keep each pull request focused on one problem so its effect can be reviewed.

This is a **source-available** repository with no project-wide open-source license
grant. Public access does not grant broad permission to use, modify or redistribute
the project. Third-party materials retain their respective licenses; see
[third-party notices](docs/licenses/third-party.md). Discuss licensing with the
owner before a substantial contribution. A contribution does not change the
repository's licensing status.

Use [Support](SUPPORT.md) for ordinary failures and [Security](SECURITY.md) for
suspected vulnerabilities. Security reports belong in the private reporting
channel.

## Working on a change

1. Create a branch from the public repository's `main` branch.
2. Follow [Getting started](docs/getting-started.md) to prepare the Windows x64
   toolchain and dependencies.
3. Make the smallest coherent change. Explain the reason for non-obvious resource
   ownership, synchronization or timing decisions in nearby comments.
4. Run the checks relevant to the changed component, then describe the result in
   the pull request. See [Testing](docs/testing.md) for the full verification
   workflow and hardware requirements.

Frontend checks can be run from the repository root:

```powershell
npm ci
npm run typecheck -w @rebelliocap/desktop
npm run lint -w @rebelliocap/desktop
npm run test -w @rebelliocap/desktop
npm run build -w @rebelliocap/desktop
```

The current `lint` script runs the TypeScript compiler. Browser acceptance tests
are separate:

```powershell
npx playwright install chromium
npm run test:e2e -w @rebelliocap/desktop
```

For native engine or Rust host changes, follow the native configure/build/staging
steps in [Getting started](docs/getting-started.md) and the commands in
[Testing](docs/testing.md). Changes to recording settings may affect C++, Rust and
TypeScript; check the shared contract in `contracts/recording-settings.v1.json`
and its tests across those layers.

## Verification and review

- Add a regression test when a behavioral change has a practical, meaningful
  automated check. Documentation-only changes need link and factual review.
- Separate deterministic unit tests, browser tests, native desktop checks and
  hardware/performance evidence. A browser test using a mock host does not prove
  real capture or encoding works.
- For capture, audio or recovery changes, report the Windows version, GPU/driver,
  display/audio configuration, test duration and settings used. Mark unavailable
  checks as **not run** and explain why.
- Keep generated recordings, logs, traces, screenshots, credentials and personal
  paths out of commits. Share only reviewed, minimal evidence; see
  [Privacy and local data](docs/privacy.md).
- Preserve dependency notices and lockfiles when changing dependencies.

A pull request should explain the user-visible problem, the resulting behavior
and the verification performed. Include screenshots for visible interface
changes when they help review, using neutral content without personal data.
Be specific and respectful in reports and reviews. Review and acceptance depend
on maintainer availability; there is no promised turnaround time.
