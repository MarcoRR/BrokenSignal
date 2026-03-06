# BROKEN SIGNAL

Audio player for the **M5Stack Cardputer ADV**. Plays MP3 and M4A files from an SD card with folder navigation.

![Screenshot](images/BrokenSignal.png)

-> Support: [Ko-fi](https://ko-fi.com/marcorr)  

---

## Features

- **MP3 and M4A (AAC-LC) playback** — native iTunes M4A support via a custom MP4 demuxer, no conversion needed
- **Folder navigation** — full subfolder tree under `/Music/`, with lazy scanning so startup is instant
- **Large folder support** — folders with 200+ tracks paginated in pages of 25
- **Five themes** — switch live with keys 1–5
- **Repeat modes** — off / one / all
- **Shuffle**
- **Recent tracks** — virtual folder showing the last 10 played
- **Persistent settings** — theme, volume, repeat, and shuffle saved to `/Music/settings.cfg` between reboots
- **Screen off** — option to turn off display power while audio continues
- **Help overlay** — press H at any time

---

## Themes

| Key | Name | Palette |
|-----|------|---------|
| `1` | Neon Noir | Magenta + cyan on dark background |
| `2` | Glitch Terminal | Phosphor green CRT, amber accents |
| `3` | Corpo Chrome | Gold + chrome on dark slate |
| `4` | Miami Vice | Hot pink + turquoise on dark navy |
| `5` | Ash | Monochrome white-on-black |

There are a few **screenshots** the bottom of this readme.

---

## Controls

| Key | Action |
|-----|--------|
| `;` / `.` | Cursor up / down |
| `ENTER` | Open folder · Play track · Press again to stop |
| `DEL` | Back to parent folder |
| `SPACE` | Pause / Resume |
| `,` | Prev track (playing) · Prev page / parent folder (browsing) |
| `/` | Next track (playing) · Next page (browsing) |
| `+` / `=` | Volume up |
| `-` | Volume down |
| `R` | Cycle repeat mode (off → one → all) |
| `S` | Toggle shuffle |
| `O` | Screen on / off |
| `H` | Help overlay |
| `1`–`5` | Switch theme |

---

## Hardware

- **M5Stack Cardputer ADV**
- **MicroSD card**

---

## SD Card Layout

```
SD/
└── Music/
    ├── track.mp3
    ├── track.m4a
    ├── settings.cfg        ← auto-created on first save, you don't need to add this manually
    └── Album Folder/
        ├── 01 - Track.mp3
        └── 02 - Track.m4a
```

Subfolders nest to any depth. Files outside `/Music/` are ignored.

---

## Installation from .bin

### M5Launcher
Download the .bin from releases, copy to your sd card and install it via [M5 Launcher](https://github.com/bmorcelli/Launcher)

## Build and upload via Arduino IDE

### Dependencies

Install both libraries via the Arduino Library Manager or the links below:

| Library | Source |
|---------|--------|
| M5Cardputer | https://github.com/m5stack/M5Cardputer |
| ESP8266Audio | https://github.com/earlephilhower/ESP8266Audio |

### Board setup

1. In Arduino IDE, add the ESP32 board package from Espressif
2. Select **M5Stack Cardputer** as the target board
3. Set **Partition Scheme** to one with enough app space (e.g. *Huge APP*)

### Flash

1. Clone or download this repo
2. Open `BrokenSignal.ino` in Arduino IDE
3. Connect the Cardputer via USB-C
4. Upload

---

## Screenshots

![Glitch Terminal theme](images/cardputer.png)
![Glitch Terminal theme](images/theme2.png)
![Corpo Chrome theme](/images/theme3.png)
![Miami Vice theme](images/theme4.png)


---

## Technical notes

**M4A playback** — The player includes a hand-written MP4 container demuxer (`AudioFileSourceM4A`) that parses the `moov` atom tree, extracts the AAC sample table, and streams frames with ADTS headers prepended for `AudioGeneratorAAC`. Duration reading uses an end-of-file first strategy to avoid walking the FAT chain past large `mdat` blocks.

**Memory** — Each scanned folder uses ~12KB RAM: a 200-entry name cache (~10KB) plus a 25-entry duration cache for the current page (~2KB). An 11-slot LRU evicts the least recently used folder when memory runs low. The AAC SBR decoder needs ~50KB contiguous; the LRU evicts additional folders before opening an M4A file if the heap is below 80KB.

**Battery** — The main loop uses `delay(1)` while playing (yields to the RTOS speaker-DMA task) and `delay(10)` while idle (~50× less CPU). The status bar and header only redraw when something actually changes.


---

## License

AI models were heavily used to create this code. You may want to hire a team of lawyers to determine what kind of licence it ends up falling under. If you do, let me know — I genuinely have no clue.

As for my personal preference: simply mention the name of this repository and link back to it if you reuse or redistribute this code.
