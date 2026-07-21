# TG2SIP-WebRTC

A fork of [Infactum/tg2sip](https://github.com/Infactum/tg2sip) (archived,
read-only) updated to use Telegram's current WebRTC-based calling protocol
([`pytgcalls/ntgcalls`](https://github.com/pytgcalls/ntgcalls)) instead of
the now-defunct `libtgvoip` P2P protocol, plus a matching TDLib version bump
(the old pinned TDLib was refused login by Telegram's servers entirely,
independent of the calling-protocol change).

TG2SIP is a Telegram<->SIP voice gateway. It can be used to forward incoming telegram calls to your SIP PBX or make SIP->Telegram calls.

## Requirements

Your SIP PBX should be comaptible with `L16@48000` or `OPUS@48000` voice codec.

## Usage

1. Clone with submodules and build from source (no prebuilt binaries/AppImage
   for this fork yet - see `.github/workflows-disabled/`):
   ```
   git clone --recurse-submodules <this repo>
   ./buildenv/build-clang-libcxx-deps.sh
   cmake --preset clang-libcxx
   cmake --build build-clang
   ```
   Requires Clang with libc++ (not GCC/libstdc++ - the `ntgcalls` calling
   backend ships a prebuilt WebRTC static library that's unconditionally
   compiled with Clang+libc++, so the whole toolchain has to match; Clang
   19+ is the practical floor, Clang 20+ is what libc++ itself claims to
   officially support and is warning-free). `buildenv/build-clang-libcxx-deps.sh`
   rebuilds PJSIP, TDLib, and spdlog from source under that same toolchain
   into a private prefix first (see `buildenv/` for the exact versions/
   patches this fork builds against) - required before the `cmake --preset`
   step above, since none of Debian's own apt packages for these are
   ABI-compatible with `ntgcalls`' prebuilt WebRTC.

2. The build drops a template at `build-clang/tg2sip.conf.sample` (never overwritten in place - a fresh `cmake --build` always regenerates this exact file, so a real config never lives here). Copy it to one of the paths `tg2sip-webrtc` actually reads, in priority order:
   1. An explicit path passed as the first command-line argument: `tg2sip-webrtc /path/to/tg2sip.conf`
   2. `/etc/tg2sip-webrtc/tg2sip.conf` (recommended for a real install - `sudo mkdir -p /etc/tg2sip-webrtc && sudo cp build-clang/tg2sip.conf.sample /etc/tg2sip-webrtc/tg2sip.conf`)
   3. `tg2sip.conf` in the current working directory (dev/in-tree convenience fallback - `cp build-clang/tg2sip.conf.sample build-clang/tg2sip.conf`, then run from `build-clang/`)
3. Obtain `api_id` and `api_hash` tokens from [this](https://my.telegram.org) page and put them in your config file.
4. Login into telegram with `tg2sip-gendb` app (same config-resolution rule as above)
5. Set SIP server settings in your config file
6. Run `tg2sip-webrtc`

SIP->Telegram calls can be done using 3 extension types:

1. `tg#[\s\d]+` for calls by username
2. `\+[\d]+` for calls by phone number
3. `[\d]+` for calls by telegram ID. Only known IDs allowed by telegram API.

All Telegram->SIP calls will be redirected to `callback_uri` SIP-URI that can be set in from `tg2sip.conf` file.  
Extra information about caller Telegram account will be added into `X-TG-*` SIP tags.
