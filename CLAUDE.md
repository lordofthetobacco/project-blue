# Multi-Agent Coordination & Orchestration (`hcom`)

This project uses **`hcom`** (`https://github.com/aannoo/hcom`) as a real-time message bus, SQLite-backed event stream, transcript sharing layer, and agent lifecycle manager across terminals and machines.

When started via `hcom claude`, hooks automatically bind your session to the `hcom` bus. Incoming messages, collision warnings, and subscribed events are injected directly into your context (both mid-turn after tool calls and when idle).

---

## 1. Core Operational Rules

1. **Discover Before Acting**: At the start of a multi-agent task or when asked to collaborate, run `hcom list` to see active sibling agents, their status (`active`, `listening`, `inactive`), and their roles/tags.
2. **Respect File Ownership & Collisions**: `hcom` monitors file edits across all running agents and triggers collision alerts if two agents modify the same file within 20 seconds.
   - If you receive a collision notification, **halt edits to that file immediately**.
   - Run `hcom send @<other-agent> --intent request -- "I noticed we both touched <file>. Which sections are you owning?"` to Partition the work before continuing.
3. **Structured Messaging**: Always include `--intent` (`request`, `inform`, `ack`, `error`) and `--thread <topic>` when coordinating complex tasks so agents and event filters can track state cleanly.
4. **Context Handoffs Over Copy-Paste**: Instead of pasting massive code blocks or logs into messages, use `hcom transcript` or `hcom bundle` to share structured context slices and event ranges.
5. **Clean Up Spawned Workers**: If you spawn background/headless sub-agents (`hcom claude --headless` or `-p`) or temporary workflow clones, terminate them via `hcom kill <name> --go` once their task is verified and merged.

6. Remeber to stick to the guidelines of clean coding that were made with a little help of the friends from google. They are in .claude/rules folder.

---

## 2. CLI Quick Reference

If you ever need exact flag syntax for the installed version of `hcom`, run:
```bash
hcom --help
hcom <subcommand> --help
hcom run docs   # Prints full CLI, configuration, and API reference

