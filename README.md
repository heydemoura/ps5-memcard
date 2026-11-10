# ps5-memcard

A memory card for a jailbroken PS5. The payload watches every save on the console
(PS5 and PS4 titles, all profiles) and keeps a copy on a USB drive under `/memcard`.
Its web page shows what is on the card, imports any card save into any profile on
the console, and backs up a save on demand.

![The page](docs/screenshot.png)

Built on [Garlic SaveMgr](https://github.com/earthonion/garlic-savemgr) by earthonion:
the native save mounting, re-signing and import code is Garlic's. The page follows
the look of [sendpayload.pages.dev](https://sendpayload.pages.dev).

## Use

1. Plug a USB drive (exFAT or FAT32) into the PS5.
2. Send `ps5-memcard.elf` to the ELF loader (port 9021).
3. Open `http://<ps5-ip>:8083/`.

From then on it runs by itself until the console reboots. Sending the payload again
replaces the running copy.

| Tab | What you get |
| --- | --- |
| **On the card** | Every save on the USB drive, grouped by game, with the profile it came from. **Import** copies it into the profile you pick. **✕** deletes it from the card. |
| **On this console** | Every save on the console and whether it is on the card yet. **Back up** copies one now, **Back up game** / **Back up all** do the same in bulk. |
| **Status** | Counters, the running copy, an activity log, and the settings. |

## How the backup behaves

- The console is checked every 5 s. A save is copied once it has been unchanged for
  15 s (*quiet time*), so a save that is still being written is left alone.
- While a game is running, the same save is copied at most every 5 min (*in-game
  gap*). When the game closes its saves are copied right away, and compared byte by
  byte in case the file changed without a new timestamp.
- A copy is written next to the old one and swapped in only when complete. If the
  save changed while it was being copied, the copy is thrown away and retried.
- The previous copy of each save is kept as `<save>.prev` and can be imported too.
- An older console save never replaces a newer card copy by itself (shown as
  *card is newer*). Backing it up by hand overrides that.
- Nothing is written unless `/mnt/usbN` is a real mounted drive.
- One console notification per pass, none over a running game.

All of this is in Settings on the page, stored in `/data/memcard/config.json`.

## Card layout

```
/memcard/index.tsv                      what was copied, from which save state
/memcard/<user id>/profile.txt          name of the profile
/memcard/<user id>/<TITLE>/<save>       the save image, byte for byte
/memcard/<user id>/<TITLE>/<save>.prev  the copy before that
/memcard/<user id>/<TITLE>/<name>.bin   PS4 sealed key that belongs to the image
```

A decrypted save folder (with `sce_sys/`, as exported by Garlic SaveMgr together
with its `chives.json`) dropped into `/memcard/<user id>/<TITLE>/` is listed and
can be imported as well.

Importing never touches the card: the save is copied to `/data/memcard/import`,
re-signed for the chosen profile there, and installed from there. Importing the
save of a game that is running is refused.

## API

Everything the page does, on port 8083:

| | |
| --- | --- |
| `GET /api/mc/status` | drive, counters, running copy, activity, settings, profiles |
| `GET /api/mc/card` | saves on the card |
| `GET /api/mc/console` | saves on the console and their state |
| `POST /api/mc/backup?uid=&title=&save=` | queue a backup; any part may be left out, or `all=1` |
| `POST /api/mc/import?id=<uid/TITLE/save>&uid=<profile>[&previous=1]` | import a card save |
| `POST /api/mc/delete?id=<uid/TITLE/save>` | delete a card save |
| `GET` / `POST /api/mc/settings` | read / change settings (JSON body) |
| `GET /api/mc/icon?title=<TITLE>` | game icon |

Garlic SaveMgr's own endpoints (`/api/saves`, `/api/log`, `/api/mount`, ...) are
still there; its page, cloud/FTP watcher and UI are not used.

## Build

Needs the [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk) and a host
clang/lld.

```sh
make PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk     # -> ps5-memcard.elf
```

## Test without a console

The memory card logic (`src/memcard.c`) is plain POSIX and runs on a PC.

```sh
make test          # watcher / index / staging rules against a fake tree
make sim           # the real page + the real core on http://127.0.0.1:8083/
npm install && make build/sim build/ui.html && npx playwright test   # browser tests
```

`make sim` fakes only the import step (that needs the console's save crypto).

## Tested on a console

On a PS5 with a USB exFAT drive: drive detection, automatic mirror (card copy
byte-identical to the console save), previous-copy rotation, manual backup, import
of a decrypted folder, import of a mirrored image (the installed save mounts and
holds its files), delete, and replacing a running instance.

Not yet exercised on hardware: PS4 saves, and moving a card between two consoles.

## Layout of the source

- `src/memcard.c`, `src/memcard.h`: the memory card (watcher, index, staging, `/api/mc/*`).
- `src/ui.html`: the page, embedded into the payload at build time.
- `src/main.c`, `src/CMP/`: Garlic SaveMgr, with the glue marked `memcard glue`.
- `tests/`: host test, simulator, browser tests.

## Credits

Garlic SaveMgr by earthonion and contributors. Upstream ships without a license
file, which is one reason this repository is private.
