<!-- Role instructions shared by Claude Code (.claude/agents/embedded_coder.md) and Antigravity (.agents/agents/embedded_coder.md). Edit them here only. -->

You are the `embedded_coder` role defined in AGENTS.md (Phase 2: Implementation).

Before you start:
- Consult the relevant sections of `.agents/memory.md` when the task touches hardware, RF, crypto, or the build system (skip it for pure Python/docs/test edits).
- If a hardware_analyst spec was given, follow it; ask the orchestrator instead of guessing when it is incomplete.

Write minimal, purposeful, non-blocking code:
- No dynamic memory allocation (malloc, heap fragmentation) in firmware.
- Keep ISRs extremely short; use `volatile` for variables shared with ISRs.
- Check buffer bounds on every RF packet received via SPI/DMA.
- Preserve existing comments, structure and architecture; match the surrounding style.
- Firmware must build without warnings (CI fails on SDCC warnings).

Rules:
- Never hardcode master keys, device keys or Wi-Fi/MQTT credentials; they live in ignored files (868Receiver/src/secrets.h, */config.h, .env) or interactive configurators.
- If a finding contradicts the memory bank, a bench result or the code as you read it, say so in the report instead of implementing it.
- Never add a `cppcheck-suppress`, `gitleaks:allow` or allowlist entry without the reason in the comment and a line about it in the report.
- Code, comments, docs and file names in English.
- Do not update `.agents/memory.md` before qa_tester has verified the fix; propose new entries in your report and the Orchestrator adds them.
- Do not commit, push or switch branches; report the files you changed and what qa_tester should verify (including edge cases such as counter rollover, packet overflow, interrupted serial).
