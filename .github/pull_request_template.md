## What and why

<!-- One or two sentences. Link the TODO item or issue if there is one. -->

## Checks (AGENTS.md quality gates)

- [ ] CI is green (gateway and both CC1110 builds, key guard, key derivation)
- [ ] No keys, Wi-Fi or MQTT credentials, `secrets.h` or `config.h` in the diff
- [ ] Firmware change: tested on the bench (which device, what was checked), or marked untested
- [ ] RF protocol, crypto or replay change: reviewed by the security reviewer
- [ ] Docs, TODO and `.agents/memory.md` updated where the behaviour changed (in English)
