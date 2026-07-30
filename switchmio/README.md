# switchmio

A minimal Nintendo Switch homebrew app that opens **Stremio's web app**
(https://web.stremio.com/) using the console's built-in system Web Applet.
Runs fine under Atmosphere's Homebrew Menu.

## Project layout
```
switchmio/
├── Makefile
├── source/
│   └── main.c
└── icon/
    └── icon.jpg
```

## 1. Build requirements
You need **devkitPro** installed on your PC with the Switch dev packages
(devkitA64, libnx, switch-tools). Install from:
https://devkitpro.org/wiki/Getting_Started

Important gotchas that trip people up on Windows:
- Install to a path **with no spaces**, e.g. `C:\devkitPro` (NOT
  `C:\Program Files\...` or anything under a folder with spaces).
- Put your project folder somewhere with no spaces too, e.g.
  `C:\dev\switchmio`.
- After installing, **fully close and reopen** PowerShell so the new
  environment variables (`DEVKITPRO`, `DEVKITA64`, PATH entries) take effect.
- Verify before building:
  ```powershell
  echo $env:DEVKITPRO
  echo $env:DEVKITA64
  aarch64-none-elf-gcc --version
  ```
  All three should return real output, not blank/errors.

## 2. Build
```powershell
cd C:\dev\switchmio
make
```
This produces **`switchmio.nro`** in the project folder.

## 3. Install on your Switch
Copy `switchmio.nro` to your SD card at:
```
sdcard:/switch/switchmio/switchmio.nro
```
Boot into Atmosphere, open the **Homebrew Menu (hbmenu)**, and you'll see
"switchmio" with its icon. Select it to launch straight into Stremio's web app.

Press **+** or use the web applet's close control to exit back to hbmenu.

## Changing the URL later
Edit this line in `source/main.c`:
```c
#define WEBSITE_URL "https://web.stremio.com/"
```
then run `make clean` followed by `make` to rebuild.

## Notes
- Uses libnx's official `webPageCreate` / `webConfigShow` API — the standard,
  documented way homebrew opens URLs. No exploits or patches involved.
- Requires the Switch to have Wi-Fi/internet access to actually load the page.
- Stremio's web app needs you to sign in / connect to your Stremio account
  the same way you would in a desktop browser — this app just opens the page,
  it doesn't do anything special beyond that.
