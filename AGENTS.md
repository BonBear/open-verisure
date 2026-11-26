# Multi-Agent Engineering Architecture & Guidelines

Collaboration rules, quality gates, and token-efficiency guardrails for **OpenVerisure** (read by both **Claude Code** and **Google Antigravity**; tool-specific details live in [documentation/agent_workflow.md](documentation/agent_workflow.md)).

---

## 1. Roles & Model Tiers

| Role | Subagent | Model Tier | Permissions | Responsibility |
| :--- | :--- | :--- | :--- | :--- |
| **Orchestrator** | Main session | strong | All tools | Plans work, delegates, runs git/PRs, synthesizes answers. |
| **Hardware & Protocol Analyst** | `hardware_analyst` | strong (high thinking) | Read-only | Datasheets, pinmux/DMA, 868.950 MHz RF, AES-128 KDF specs. |
| **Embedded Coder** | `embedded_coder` | fast *(strong for complex logic)* | Read/Write/Edit | Minimal, non-blocking C/C++ (PlatformIO, SDCC, MSP430) & Python. |
| **QA & Test Runner** | `qa_tester` | fast | Execution (read-only files) | Targeted builds (`pio`, SDCC) & tests; never edits or flashes. |
| **Security & Code Reviewer** | `code_reviewer` | risk-tiered (`pro`/`opus` or `flash`/`sonnet`) | Read-only | Independent audit of memory safety, ISRs, crypto, replay, secrets. |

Edit shared role instructions only in `.agents/roles/<role>.md` (never in `.claude/agents/` or `.agents/agents/`).

---

## 2. Workflow & Quality Gates

1. **Phase 1 — Spec (`hardware_analyst`):** Use before implementing changes to RF protocol, crypto/KDF, peripherals, registers, or ESP32-S2 pinmux (ADC1 vs Wi-Fi coexistence).
2. **Phase 2 — Implement (`embedded_coder`):** Minimal, non-blocking code; no `malloc`/heap fragmentation in firmware; short ISRs with `volatile` shared state; preserve existing comments/style.
3. **Phase 3 — Verify (`qa_tester`):** Run only the fast, targeted build/tests for the changed component (warning-free `pio`/SDCC, affected `pytest`/host tests, `check_secrets.py`, and `cppcheck` when installed). Let GitHub Actions run the full matrix.
4. **Phase 4 — Review (`code_reviewer`, risk-tiered):**
   - **Sensitive changes** (RF protocol, crypto/KDF/replay, ISRs/DMA, flash/counter log, firmware memory safety, credentials/auth, CI/security gates): run `code_reviewer` on `pro`/`opus`.
   - **Low-risk changes** (helper/bench scripts, portal UI, LUT/constant tweaks, build tooling): run `code_reviewer` on `flash`/`sonnet` (Claude Code: `effort="medium"`; 1 concise pass).
   - **Pure docs, comments, or test-only additions:** no `code_reviewer` subagent needed; CI + Orchestrator inspection suffice.

### Fast Path for Small Low-Risk Changes
For focused, unambiguous changes (`< ~30` lines outside RF framing, crypto/replay, ISRs/DMA, flash log, or auth) where the Orchestrator already has the file in context, the Orchestrator may edit directly and run the targeted build/test instead of spawning `embedded_coder` + `qa_tester`, then apply Phase 4 risk-tiered review + CI.

### Round Limits & Finding Verification
- **Max 3 rounds** per change (1 round = Coder -> QA -> Reviewer, or a QA/CI failure sent back to Coder). After round 3, stop and hand open points to the user.
- **Verify before acting** on any finding that contradicts a bench result, memory entry, or earlier conclusion, or that changes sensitive code: confirm with a failing host test, log, bench check, or independent second reviewer before sending to Coder. Clear-cut findings (typos, comment mismatches) go to Coder directly.

---

## 3. Guardrails & Token Efficiency

1. **No secrets in Git:** Never hardcode or commit master keys, device keys, or Wi-Fi/MQTT/portal credentials (keep in ignored `secrets.h`, `config.h`, `.env`).
2. **Read-only reviewers:** `hardware_analyst` and `code_reviewer` never modify files or run commands. Save diffs under `scratch/` (git-ignored) and pass the path.
3. **English in the repository:** All committed code, comments, docs, TODOs, memory entries, and filenames are in English.
4. **Deterministic CI gates first:** CI runs warning-free builds, `ruff`, release check, `check_secrets.py`, `gitleaks`, `cppcheck`, and CodeQL. Reviewers do not repeat these, but **always** audit changes that weaken gates (new `cppcheck-suppress`, `gitleaks:allow`, allowlists, workflow edits) plus credentials and 16-bit MSP430/CC1110 arithmetic.
5. **Reuse warm subagents:** Reuse an existing `embedded_coder`, `qa_tester`, or `code_reviewer` in the same component area via messaging (`send_message`) across tasks and rounds instead of spawning cold subagents.
6. **Minimal context & quiet logs:**
   - Pass explicit file paths, diff paths, and round numbers to subagents; ask for concise replies.
   - Avoid dumping repeated polling tables into the main thread (e.g. use `gh pr checks <n> --watch --interval 30` or summarize final status).
   - Between major milestones, update `.agents/handover.md` (git-ignored) and compact/rotate the session.
7. **Git, docs & PR ownership:** Only the Orchestrator commits, pushes, and opens PRs; `main` is protected (every change goes through a PR with green CI). Before pushing a branch or opening a PR, update all affected repository documentation (`README.md`, component `README.md`s, and `documentation/*.md`) so docs ship in the same PR as the code. Read `.agents/handover.md` at session start if present.

---

## 4. Long-Term Memory (`.agents/memory.md` + `.agents/memory/`)

The memory bank is split by topic under `.agents/memory/` with a small index at [memory.md](.agents/memory.md):
- **Selective read:** Before hardware/protocol/build work, check `.agents/memory.md` (index) and read **only the 1–2 matching topic files** — never load unrelated components.
- **Format:** `- [YYYY-MM-DD] [AREA/COMPONENT]: Concrete rule, limitation or lesson learned.` Confirmed facts only; no code blocks or raw logs; rewrite superseded entries.
- **Updating:** Orchestrator (or `embedded_coder`) updates the matching topic file after `qa_tester` verifies the fix.
