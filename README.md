# spotc

Spotify client for the terminal, written in C. Browse your library, control any
Spotify Connect device, and play locally through a built-in slowed + reverb
effect chain.

Requires Spotify Premium.

```
┌ spotc ──────────────────────────────────────── ◈ spotc ┐
│ ♥ Liked Songs   │  Liked Songs · 212 tracks            │
│ ⊚ an album      │    1  Track one        Artist   3:41 │
│ ≡ a playlist    │    2  Track two        Artist   4:05 │
│ ...             │  ...                                 │
├─────────────────────────────────────────────────────────┤
│ ▶ Track one — Artist                       1:23 / 3:41  │
│ ━━━━━━━●─────────────────────────────────────────────── │
│ vol 80% · 0.86x slow · rev 18%                  ? keys  │
└─────────────────────────────────────────────────────────┘
```

## Features

- Liked songs, saved albums and playlists in a scrollable sidebar (paginated,
  not capped at the API's 50-per-page)
- Search for tracks and albums
- Full playback control over Spotify Connect: play, seek, volume, shuffle,
  repeat, queue, device switching
- Local playback device via librespot, so sound comes out of the machine
  spotc runs on
- Slowed + reverb: a cubic-Hermite resampler (pitch drops with speed, as it
  should) and a Freeverb implementation, applied live to the local audio with
  smooth transitions — 8 gradual levels or exact values in the config
- Every key rebindable in a plain-text config, 256-color accent theming

## How it works

```
spotc (ncurses UI, Web API client)
└─ librespot --backend pipe  →  spotc-fx (DSP)  →  pacat (PulseAudio/PipeWire)
```

`spotc-fx` is a separate ~16KB binary that reads raw s16le audio on stdin and
writes it processed to stdout; spotc controls it at runtime through a FIFO.
The effects only apply to the local `spotc` device — Spotify's API does not
expose audio from other Connect devices.

`spotc-ipv4.so` is preloaded into librespot to force IPv4 name resolution:
routers that advertise IPv6 without working upstream otherwise break
librespot's websocket connection (`Network is unreachable`) and the local
device never appears. Harmless when IPv6 works.

## Dependencies

- ncursesw, json-c, libcurl, openssl (build)
- [librespot](https://github.com/librespot-org/librespot) ≥ 0.8 —
  `cargo install librespot --locked` (without `--locked` the build fails)
- `pacat` (pulseaudio-utils; PipeWire's pulse compatibility works fine)

## Build

```sh
make
make install     # installs to ~/.local/bin
```

## Setup

Spotify removed shared-app access to the Web API, so you need your own (free)
API key — one-time, about two minutes:

1. Open <https://developer.spotify.com/dashboard> → Create app.
   Any name; Redirect URI: `http://127.0.0.1:5588/login`; check "Web API".
2. Run `spotc`, press `i`, paste the Client ID.
3. Press `l` and sign in in the browser.

For local audio (and the slow/reverb effects), pair the device once: open
Spotify on your phone → Devices → pick `spotc`.

## Keys

| Key | Action | Key | Action |
|-----|--------|-----|--------|
| `j` `k` | navigate | `Enter` | play / open |
| `Tab` | switch pane | `Space` | play/pause |
| `n` `b` | next / previous | `,` `.` | seek ±10s |
| `-` `+` | volume | `a` | add to queue |
| `s` | shuffle | `r` | repeat |
| `o` | slow+reverb on/off | `p` | next of 8 levels |
| `0` | reset fx | `/` | search |
| `d` | devices | `?` | help |
| `R` | reload library | `X` | logout |
| `q` | quit | | |

All of them can be changed in `~/.config/spotc/config`, which also holds
colors, bitrate, device name and persisted fx values.

## Notes

- Tokens are stored in `~/.config/spotc/tokens.json` (0600), librespot's cache
  in `~/.cache/spotc/`.
- `token-helper/` is an optional Rust experiment that mints Web API tokens
  through the librespot session. The main flow doesn't use it; it's kept for
  reference and not built by the Makefile.

## License

MIT
# spotc
