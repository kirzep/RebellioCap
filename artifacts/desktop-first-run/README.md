# RebellioCap First-Run Desktop Evidence Artifacts

This directory houses the authoritative verification evidence, test reports, and visual proof for the RebellioCap desktop first-run application.

## Manifest and Verification
- Schema: [`manifest.schema.json`](./manifest.schema.json)
- Generate & Verify: `powershell -NoProfile -File scripts/verify-desktop.ps1`
- Hardware Run: `powershell -NoProfile -File scripts/verify-desktop.ps1 -Hardware -EvidenceRoot artifacts/desktop-first-run`

## Required Artifacts Checklist
- [x] Static identity and brand check (`scripts/verify-rebelliocap-identity.ps1`)
- [x] Rust unit and integration tests (`cargo test --manifest-path apps/desktop/src-tauri/Cargo.toml`)
- [x] C++ engine builds and tests (`ctest --preset windows-debug`)
- [x] React 19 / TypeScript typecheck and Vitest suite (`npm run test -w @rebelliocap/desktop -- --run`)
- [x] Production build bundle (`npm run build -w @rebelliocap/desktop`)
- [x] Playwright E2E suite (`apps/desktop/e2e/*.spec.ts`)
- [x] CSP and offline security verification
