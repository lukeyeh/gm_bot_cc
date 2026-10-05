# gm_bot

A fast Discord bot that tracks daily "GM" streaks.

With its ledger on disk it handles about **11,600 events a second** on one
thread, answers a GM in about **a third of a millisecond**, starts in 10 ms
and runs in about 20 MB. It is written in C++20 from the socket up: its own
Discord client, WebSocket, HTTP, TLS layering and JSON, on io_uring.

## What it does

The bot watches a GM channel in each server it serves. A GM channel is for
saying GM and nothing else:

- Someone says a GM phrase there: `gm`, `good morning` or `morning` to begin
  with, and whatever the server's admins make of the list after that. The
  bot reacts with 🌅, records it, and speaks up when there is something to
  say: a streak starting, a personal record, a whole week, a round number,
  or a GM that was already counted today.
- Someone says anything else there. The bot reacts with 👎, resets their
  streak to zero, and tells them so. Their next GM starts a new one.

Each server has its own streaks, leaderboard and phrases. Commands work
anywhere in a server that has a GM channel:

| Command | Answer |
| --- | --- |
| `/leaderboard [limit]` | The standings: longest live streak first. Shows 10 people unless told otherwise, 25 at most. |
| `/streak` | Your own streak, and your best if it was longer. |
| `/gmlist` | The phrases that count as a GM. |
| `/gmadd <phrase>` | Makes a phrase count. For administrators, unless the server's settings say otherwise. |
| `/gmremove <phrase>` | Stops a phrase counting. Likewise. |

A phrase counts in any capitalisation, on its own or at the start or end of a
longer message. One can be up to 50 characters, on one line, without
backticks.

## Why it is fast

**One thread, never blocked on the network.** Every connection is driven by
coroutines on a single event loop over io_uring (epoll where io_uring is not
allowed). There are no locks, no thread hand-offs, and no callbacks: code
reads top to bottom and suspends where it would otherwise wait.

**Nothing between the bot and the kernel that it does not need.** The
Discord client is written for this bot, on a WebSocket, HTTP client and JSON
parser that do only what Discord requires. A gateway event goes from socket
to handler in about 10 µs, of which the kernel is about 5:

| Step | Cost |
| --- | --- |
| Receive a WebSocket message over loopback | 5.7 µs |
| Parse a 1 KB gateway event as JSON | 3.5 µs |
| Turn it into a typed event | 0.2 µs |
| Decide whether it says GM | 0.03 µs |

**A ledger that does a constant amount of work per GM.** Recording a GM
reads and writes a handful of rows belonging to one person, however many
people and years the ledger holds: 22–49 µs. The database writes through a
log instead of waiting on the disk each time, and compiled statements are
kept and reused.

**Data handed on, not copied.** An event's payload is moved from the parser
to the handler; strings read from the network are views into the receive
buffer until something needs to keep them.

**Measured, not assumed.** Every package has benchmarks beside its tests,
and two load tests drive the WebSocket client and the whole bot over real
sockets. The ledger was 200 times slower before those pointed at it.

## Architecture

Each directory is a layer and a Bazel package. Dependencies point downwards
only, and Bazel `visibility` enforces it.

```
main.cc
  bot/          the bot: configuration, and the loop joining the two below
  ├─ discord/   Discord, as a bot sees it: connect, next event, send, react
  │  ├─ http/        an HTTP/1.1 client
  │  ├─ websocket/   WebSocket connections
  │  │  └─ net/      byte streams: TCP, TLS, buffered reading, the event loop
  │  │     └─ os/    the kernel: sockets, io_uring, epoll
  │  └─ json/        JSON values
  └─ gm/        who said GM when, and the streaks that follow
     └─ sqlite/      SQLite databases
  async/        Task, Sequence, Awaitable, TaskScope: used by everything that
                waits
  perf/         load tests
```

Two rules shape it:

- **Only `os/` talks to the kernel**, and only `async/` touches the C++
  coroutine machinery. Everything else is written in terms of `Task<T>`
  (an asynchronous function with one result), `Sequence<T>` (one that hands
  out many, keeping its place in between), `co_await`, and the classes those
  two packages export.
- **Each format or protocol is known in one place.** Discord's JSON field
  names are in `discord/wire.cc`; the gateway's opcodes and reconnection
  rules in `discord/gateway.cc`; WebSocket framing in `websocket/`; SQL in
  `gm/ledger.cc`.

### The Discord client

```cpp
CO_ASSIGN_OR_RETURN(discord::Client client,
                    co_await discord::Client::Connect(token));
for (;;) {
  CO_ASSIGN_OR_RETURN(const discord::Event event, co_await client.NextEvent());

  if (const auto* created = std::get_if<discord::MessageCreated>(&event)) {
    CO_RETURN_IF_ERROR(co_await client.React(created->message, "👋"));
  }
}
```

That is nearly the whole interface: `Connect`, `NextEvent`, `Send`, `React`,
and `OfferCommands` / `Respond` for slash commands. Heartbeats, reconnecting,
resuming a session so that events sent during a disconnection are not lost,
and waiting out rate limits all happen inside. `NextEvent` fails only when
Discord turns the bot away for good.

### The ledger

`gm::Ledger` stores the GMs themselves, one row each, and little else.
Streaks are computed from those rows when asked for, so they cannot disagree
with the record, and nothing has to run at midnight for a streak to end.

- **Communities.** Every row says which community it belongs to, which for
  the bot means which server. `ledger.community(id)` is a view of one.
- **Forfeits delete nothing.** Each member has a "life" number and each GM
  records the life it was said in. A forfeit moves the member to their next
  life, and a streak is a run of days within one. The forfeited streak is
  over, but still counts as their best.
- **Upgrades.** The file's layout is numbered, and an older file is brought
  up to date when the bot opens it.
- **Durability.** A change survives the bot crashing or being killed. If the
  whole machine loses power the last few changes may be lost; the file is
  never left damaged.

## Tests

34 test targets, 281 tests. Every `foo.h` / `foo.cc` has a `foo_test.cc`
beside it, written to be read: each opens by saying what it demonstrates,
and each test says what behaviour it shows.

- **Nothing reaches the network.** `net/`, `http/` and `websocket/` are
  tested over loopback; `discord/` and `bot/` against `http::FakeClient` and
  `websocket::FakeServer`, which script the other side.
- **Both I/O backends.** `bazel test --config=epoll //...` runs everything on
  epoll instead of io_uring.
- **A fuzz test for the ledger.** `gm/ledger_fuzz_test.cc`, using
  [FuzzTest](https://github.com/google/fuzztest), gives the ledger and a
  deliberately simple model of it the same arbitrary sequence of GMs,
  forfeits and phrase changes across two communities, and requires them to
  agree on every answer. It found a real bug within a second of first
  running.

## Compared with the original Python bot

This replaces [a Python bot](https://github.com/lukeyeh/gm_bot) built on
discord.py with a JSON file for storage. Measured on the same machine:

| | Python | This |
| --- | --- | --- |
| Record a GM, 50 users | 351 µs | 49 µs |
| Record a GM, 5,000 users | 33.6 ms | about 50 µs |
| Is this message a GM? | 1,130 ns | 29 ns |
| Parse a 1 KB gateway event | 8.5 µs | 3.5 µs |
| Startup, before connecting | 0.31 s | 0.01 s |
| Memory | 61 MB before connecting | 21 MB connected and running |
| Read one streak | 0.16 µs | 11–41 µs |
| Leaderboard, 50 users | 7.9 µs | 1.9 ms |
| Automated tests | none | 281, plus a fuzz test |

- **Writes** are where the difference is, and it grows with the server: the
  Python bot rewrites its whole file on every GM, this one touches a few
  rows. (The 5,000-user figure for this bot is not measured at that size;
  the cost depends on one member's history, not on how many members there
  are.)
- **Reads** are faster in Python, which keeps a counter per user in memory
  where this bot queries a database. Both are far below anything a person
  would notice. What the extra microseconds buy is the full history, forfeits
  that keep the past, and answers that cannot drift from the record.
- **Safety.** A crash while the Python bot is saving leaves a truncated file,
  which it then replaces with an empty one. This ledger cannot be left
  half-written.

It also behaves differently in a few ways, on purpose:

- One bot serves several servers and keeps each one's streaks and phrases
  apart.
- The GM channels have to be named; it refuses to start without
  `GM_CHANNEL_IDS`.
- Phrases match in any capitalisation, so `GM` counts.
- A streak that has lapsed reads as zero straight away.
- Command answers are plain text rather than embeds, and name people without
  notifying them.

Not carried over yet: time windows on phrases, the weekly leaderboard post,
badges, `/resetall`, `/gmhelp`, and an importer for the old `gm_data.json`.

## Running it

Create the bot in the [Discord Developer Portal](https://discord.com/developers/applications):
under **Bot**, enable the **Message Content Intent** and copy the token; under
**OAuth2 → URL Generator**, pick the `bot` and `applications.commands` scopes
with the *View Channels*, *Send Messages* and *Add Reactions* permissions,
and open the URL to invite it to a server.

Then:

```
cp .env.example .env         # then put the token and channel in it
nix develop -c sh -c 'set -a; . ./.env; set +a; bazel run //:gm_bot'
```

| Variable | Meaning |
| --- | --- |
| `DISCORD_TOKEN` | The bot's token. Required. |
| `GM_CHANNEL_IDS` | The GM channels, one per server, separated by commas: right-click each in Discord (with Developer Mode on) and **Copy Channel ID**. Required. The older name `GM_CHANNEL_ID` still works. |
| `TIMEZONE` | Whose midnight separates one day from the next, e.g. `America/New_York`. UTC if unset. |
| `DATA_DIR` | Where to keep `gm.db`. The current directory if unset. |

That line works from any shell. With direnv it is shorter: `direnv allow`
once, and from then on the directory's environment has both the tools and the
contents of `.env`, so `bazel run //:gm_bot` is all there is to type.

- `bazel run` starts the program inside Bazel's output tree, so give
  `DATA_DIR` as an absolute path when running it that way.
- Bazel must be run inside the Nix shell; outside it, Bazel silently uses
  the host compiler.
- `--io_backend=io_uring|epoll|auto` chooses how I/O is done. The default,
  `auto`, uses io_uring where the system allows it.
- A ledger from before the bot served several servers goes, when first
  opened, to the server of the first channel in `GM_CHANNEL_IDS`.

## Everyday commands

| To | Run |
| --- | --- |
| Run all tests | `bazel test //...` |
| Run them on the epoll backend | `bazel test --config=epoll //...` |
| Run every benchmark | `perf/benchmarks.sh <label>`, which saves to `perf-reports/raw/<label>/` |
| Run one test file's benchmarks | `bazel run -c opt //gm:ledger_test -- --benchmark_filter=all` |
| Run the load tests | `bazel run -c opt //perf:bench` |
| Fuzz the ledger for a minute | `bazel run --config=fuzz //gm:ledger_fuzz_test -- --fuzz=LedgerFuzzTest.AgreesWithTheModel --fuzz_for=60s` |
| Generate `compile_commands.json` for clangd | `bazel run :compile_commands` |
| Format | `clang-format -i main.cc */*.cc */*.h` and `buildifier -r .` |
| Lint | `clang-tidy main.cc */*.cc` (after generating compile commands) |
| Build the deployable package | `nix build`, giving `result/bin/gm-bot` |

Start clangd with `--query-driver=/**/*` so that it can find Nix's system
headers.

## Conventions

- Failure travels as `absl::Status` / `absl::StatusOr`. Nothing throws; the
  build has exceptions off.
- Errors propagate with `ABSL_RETURN_IF_ERROR` / `ABSL_ASSIGN_OR_RETURN`, and
  in coroutines with `CO_RETURN_IF_ERROR` / `CO_ASSIGN_OR_RETURN` from
  `async/status_macros.h`, which are the same macros leaving with `co_return`.
- Benchmarks live at the bottom of the `_test.cc` file for the code they
  measure, and run from the same binary. Under each file's "Benchmarks"
  heading are the results of the last recorded run, with the date and the
  machine. `perf/benchmarks.sh recorded && perf/record_results.py recorded`
  refreshes them.

## Libraries

Third-party libraries come from nixpkgs, at the versions `flake.lock` pins.
`nix/deps.nix` is the one list of them (Abseil, liburing, OpenSSL, SQLite,
and GoogleTest, Google Benchmark and FuzzTest for tests), used by both builds:

- Bazel imports them through
  [rules_nixpkgs](https://github.com/tweag/rules_nixpkgs), set up in
  `MODULE.bazel`. BUILD files depend on them as `"@abseil-cpp"`, `"@openssl"`
  and so on.
- The Nix package (`nix/package.nix`) takes them from the same file.

FuzzTest is the exception to "from nixpkgs", which does not package it:
`nix/fuzztest.nix` builds it from source, with nixpkgs' compiler and against
the Abseil and GoogleTest above. Its commit is pinned in that file rather
than by `flake.lock`.

`async/` (apart from `Sequence` and the status macros), `os/` and
`net/event_loop` come from [blog.cc](https://github.com/lukeyeh/blog.cc).

### Adding one

1. Add the nixpkgs package to `nix/deps.nix`.
2. Add a `nix_pkg.file(...)` block for it in `MODULE.bazel`, copying an
   existing one, and its name to the `use_repo` line.
3. Depend on it in a `BUILD` file as `"@<name>"`.
4. If the bot itself uses it, add it to `buildInputs` and the `libraries`
   list in `nix/package.nix`, and to the `inherit` line in `flake.nix`.
