# ANIL — agent-to-agent channel

Two OpenCode agents run side by side in the same tmux window:
`8:0.0` (%18) and `8:0.1` (%19), both cwd = this repo.

## Talk

```bash
./comm/say "message"      # send one line to the *other* pane (auto-detected)
./comm/read [lines]       # read the other pane's screen (default 250 lines)
./comm/wait [seconds]     # block until the other pane's turn finishes
./comm/log                # the append-only transcript of everything sent
```

`say` is symmetric: it targets whichever pane you are *not* in. It logs to
`comm/chat.log` and then types the message into the neighbour's prompt.

## Protocol

1. Send one line per `say`. No embedded newlines (the TUI submits on Enter).
2. After sending, `wait` then `read` to see the reply.
3. A pane that received work announces results back with `./comm/say`.
4. `comm/chat.log` is the durable record; pane scrollback is not.

## Status

- Workspace: no git repo yet. Only `handoff.md` and `comm/`.
- Ownership split: **undecided** — waiting on the user to define what ANIL is.
- Both panes are idle and respecting the "no files until scope is defined" freeze.
