/*
 * ============================================================
 *  BROKEN SIGNAL — Audio Player for M5Stack Cardputer
 * ============================================================
 *
 * ── Libraries needed ──────────────────────────────────────────
 *    M5Cardputer   https://github.com/m5stack/M5Cardputer
 *    ESP8266Audio  https://github.com/earlephilhower/ESP8266Audio
 *
 * ── SD Card ───────────────────────────────────────────────────
 *    Format FAT32 — put .mp3 or .m4a files anywhere under /Music/
 *    Subfolders supported. Large folders paginated (25 items/page).
 *
 * ── Keyboard Controls ─────────────────────────────────────────
 *    ;  /  .      Cursor up / down
 *    ENTER        Open folder / Play track  (press again to stop)
 *    DEL          Back to parent folder
 *    SPACE        Pause / Resume
 *    ,  /  /      Prev / Next  (track while playing, page while browsing)
 *    +  or  =     Volume up
 *    -            Volume down
 *    R            Cycle repeat mode  (off / one / all)
 *    S            Toggle shuffle
 *    O            Screen on / off
 *    H            Help overlay
 *    1 – 5        Switch theme
 *
 * ── Themes ────────────────────────────────────────────────────
 *    1  Neon Noir          (magenta + cyan)
 *    2  Glitch Terminal    (phosphor green CRT)
 *    3  Corpo Chrome       (gold + chrome)
 *    4  Miami Vice         (hot pink + turquoise)
 *    5  Ash                (monochrome)
 * ============================================================
 */

#include <M5Cardputer.h>
#include <SD.h>
#include <AudioFileSourceSD.h>
#include <AudioFileSourceID3.h>
#include <AudioGeneratorMP3.h>
#include <AudioGeneratorAAC.h>
#include <AudioFileSource.h>
#include <AudioOutput.h>
#include <algorithm>
#include <vector>

// =====================================================================
//  AudioFileSourceM4A  — iTunes M4A (AAC-LC in MP4 container) demuxer
//
//  Parses the moov atom tree to find the audio track's sample table,
//  builds a flat (offset, size) list for every AAC frame, then
//  presents those frames as a plain byte stream for AudioGeneratorAAC.
// =====================================================================

// =====================================================================
//  AudioFileSourceM4A  — iTunes M4A (AAC-LC in MP4 container) demuxer
//
//  Parses the moov atom to extract stco/stsc/stsz tables.
//  chunkOffsets (stco) are cached in RAM — tiny (chunkCount * 4 bytes).
//  sampleSizes  (stsz) are loaded after AudioGeneratorAAC is constructed,
//  so the malloc doesn't compete with the SBR decoder's 50KB block.
//  Falls back to per-frame SD seeks if the deferred malloc also fails.
//  ADTS headers are prepended to every frame for AudioGeneratorAAC.
// =====================================================================
class AudioFileSourceM4A : public AudioFileSource {
public:
    AudioFileSourceM4A() : _ok(false) {
        memset(&_st, 0, sizeof(_st));
        memset(&_cur, 0, sizeof(_cur));
    }

    bool open(const char* path) {
        _f = SD.open(path);
        if (!_f) { Serial.println("M4A: SD open failed"); return false; }
        _ok = _parse();
        if (!_ok) { Serial.println("M4A: parse failed"); _f.close(); return false; }
        _rewind();
        Serial.printf("M4A: ok  frames=%u  totalBytes=%u\n",
                      _st.sampleCount, _st.totalBytes);
        return true;
    }

    uint32_t read(void* buf, uint32_t len) override {
        uint8_t* out   = (uint8_t*)buf;
        uint32_t total = 0;
        while (total < len && _cur.sampleIdx < _st.sampleCount) {
            // Phase 0: emit ADTS header at frame start
            if (_st.adtsValid && _cur.posInFrame == 0) {
                uint8_t hdr[7];
                _makeAdtsHeader(hdr, _cur.frameSize);
                uint32_t hdrRem = 7 - _cur.adtsBytesOut;
                uint32_t hCopy  = min(hdrRem, len - total);
                memcpy(out + total, hdr + _cur.adtsBytesOut, hCopy);
                total             += hCopy;
                _cur.adtsBytesOut += hCopy;
                if (_cur.adtsBytesOut < 7) break;
            }
            // Phase 1: emit AAC payload bytes
            uint32_t frameRem = _cur.frameSize - _cur.posInFrame;
            uint32_t chunk    = min(frameRem, len - total);
            if (chunk == 0) break;
            _f.seek(_cur.frameOffset + _cur.posInFrame);
            uint32_t got = _f.read(out + total, chunk);
            total           += got;
            _cur.posInFrame += got;
            _cur.streamPos  += got;
            if (_cur.posInFrame >= _cur.frameSize) {
                _cur.sampleIdx++;
                _cur.posInFrame   = 0;
                _cur.adtsBytesOut = 0;
                _advanceFrame();
            }
            if (got < chunk) break;
        }
        return total;
    }

    bool seek(int32_t pos, int dir) override {
        if (dir == SEEK_SET && pos == 0) { _rewind(); return true; }
        return false;
    }
    bool     close()   override { _f.close(); _ok = false; return true; }
    bool     isOpen()  override { return _ok; }
    uint32_t getSize() override { return _st.totalBytes; }
    uint32_t getPos()  override { return _cur.streamPos; }

    // Always use windowed cache — predictable 1KB RAM regardless of file length.
    bool loadSampleSizes() {
        if (_st.defaultSize) return true;
        _fillWindow(0);
        return true;
    }

private:
    // ── Sample table — chunkOffsets in RAM, sampleSizes windowed ──────
    static const uint32_t STSZ_WIN = 256;  // window size in frames (1KB RAM)
    struct SampleTable {
        uint32_t* chunkOffsets;
        uint32_t  chunkCount;
        uint32_t  stszDataOffset;
        uint32_t  sampleCount;
        uint32_t  defaultSize;
        // windowed stsz cache
        uint32_t  win[STSZ_WIN];
        uint32_t  winBase;
        bool      winLoaded;
        // stsc
        static const int MAX_STSC = 64;
        struct StscEntry { uint32_t firstChunk, spc; };
        StscEntry stsc[MAX_STSC];
        uint32_t  stscCount;
        uint32_t  totalBytes;
        uint8_t   adtsSrIdx;
        uint8_t   adtsChans;
        bool      adtsValid;

        SampleTable() : chunkOffsets(nullptr), chunkCount(0),
                        stszDataOffset(0), sampleCount(0), defaultSize(0),
                        winBase(0), winLoaded(false),
                        stscCount(0), totalBytes(0),
                        adtsSrIdx(4), adtsChans(2), adtsValid(false) {}
        ~SampleTable() { free(chunkOffsets); chunkOffsets = nullptr; }
    } _st;

    struct Cursor {
        uint32_t sampleIdx;
        uint32_t posInFrame;
        uint32_t adtsBytesOut;
        uint32_t frameOffset;
        uint32_t frameSize;
        uint32_t streamPos;
        uint32_t chunkIdx;
        uint32_t sampleInChunk;
        uint32_t samplesPerChunk;
    } _cur;

    File _f;
    bool _ok;

    static uint32_t r32(File& f) {
        uint8_t b[4]; f.read(b, 4);
        return ((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|b[3];
    }
    static uint64_t r64(File& f) { uint64_t hi = r32(f); return (hi<<32)|r32(f); }
    static constexpr uint32_t FCC(char a, char b, char c, char d) {
        return ((uint32_t)(uint8_t)a<<24)|((uint32_t)(uint8_t)b<<16)
              |((uint32_t)(uint8_t)c<<8)|(uint8_t)d;
    }

    uint32_t findAtom(uint32_t base, uint32_t len, uint32_t target, uint32_t& payloadLen) {
        uint32_t cur = base, end = base + len;
        while (cur + 8 <= end) {
            _f.seek(cur);
            uint32_t sz  = r32(_f);
            uint32_t fcc = r32(_f);
            if (sz < 8) break;
            if (fcc == target) { payloadLen = sz - 8; return cur + 8; }
            cur += sz;
        }
        return 0;
    }

    // Fill the window starting at frame index `base`
    void _fillWindow(uint32_t base) {
        _st.winBase   = base;
        uint32_t n    = min(_st.sampleCount - base, STSZ_WIN);
        _f.seek(_st.stszDataOffset + base * 4);
        // Single bulk read (n*4 bytes) instead of n individual r32() calls.
        // This is one SD transaction rather than 256, safe to call mid-playback.
        uint8_t buf[STSZ_WIN * 4];
        _f.read(buf, n * 4);
        for (uint32_t i = 0; i < n; i++)
            _st.win[i] = ((uint32_t)buf[i*4]<<24) | ((uint32_t)buf[i*4+1]<<16)
                       | ((uint32_t)buf[i*4+2]<<8) |  buf[i*4+3];
        _st.winLoaded = true;
    }

    // Return sample size for frame si — full cache, window, or single SD seek
    uint32_t _sampleSize(uint32_t si) {
        if (_st.defaultSize) return _st.defaultSize;
        if (!_st.winLoaded || si < _st.winBase || si >= _st.winBase + STSZ_WIN)
            _fillWindow(si);
        return _st.win[si - _st.winBase];
    }


    uint32_t _spcForChunk(uint32_t chunkNum1) {
        uint32_t spc = _st.stsc[0].spc;
        for (int e = (int)_st.stscCount - 1; e >= 0; e--)
            if (chunkNum1 >= _st.stsc[e].firstChunk) { spc = _st.stsc[e].spc; break; }
        return spc;
    }

    void _makeAdtsHeader(uint8_t* hdr, uint32_t frameBytes) {
        uint32_t frameLen = frameBytes + 7;
        hdr[0] = 0xFF; hdr[1] = 0xF1;
        hdr[2] = (uint8_t)(((1 & 0x3) << 6) | ((_st.adtsSrIdx & 0xF) << 2)
                           | ((_st.adtsChans >> 2) & 0x1));
        hdr[3] = (uint8_t)(((_st.adtsChans & 0x3) << 6) | ((frameLen >> 11) & 0x3));
        hdr[4] = (uint8_t)((frameLen >> 3) & 0xFF);
        hdr[5] = (uint8_t)(((frameLen & 0x7) << 5) | 0x1F);
        hdr[6] = 0xFC;
    }

    void _rewind() {
        _cur.sampleIdx      = 0;
        _cur.posInFrame     = 0;
        _cur.adtsBytesOut   = 0;
        _cur.streamPos      = 0;
        _cur.chunkIdx       = 0;
        _cur.sampleInChunk  = 0;
        if (_st.sampleCount > 0) {
            _cur.samplesPerChunk = _spcForChunk(1);
            _cur.frameOffset     = _st.chunkOffsets[0];
            _cur.frameSize       = _sampleSize(0);
        }
    }

    void _advanceFrame() {
        if (_cur.sampleIdx >= _st.sampleCount) return;
        _cur.sampleInChunk++;
        if (_cur.sampleInChunk >= _cur.samplesPerChunk) {
            _cur.chunkIdx++;
            _cur.sampleInChunk   = 0;
            _cur.samplesPerChunk = _spcForChunk(_cur.chunkIdx + 1);
            _cur.frameOffset     = _st.chunkOffsets[_cur.chunkIdx];
        } else {
            _cur.frameOffset += _cur.frameSize;
        }
        _cur.frameSize  = _sampleSize(_cur.sampleIdx);
        _cur.streamPos += _cur.frameSize;
    }

    bool _parse() {
        uint32_t fsz = _f.size();

        uint32_t moovLen = 0;
        uint32_t moov = findAtom(0, fsz, FCC('m','o','o','v'), moovLen);
        if (!moov) { Serial.println("M4A: no moov"); return false; }

        // Find audio (soun) track's stbl
        uint32_t stblPos = 0, stblLen = 0;
        {
            uint32_t cur = moov, end = moov + moovLen;
            while (cur + 8 <= end) {
                _f.seek(cur);
                uint32_t sz  = r32(_f);
                uint32_t fcc = r32(_f);
                if (sz < 8) break;
                if (fcc == FCC('t','r','a','k')) {
                    uint32_t mdiaLen = 0;
                    uint32_t mdia = findAtom(cur + 8, sz - 8, FCC('m','d','i','a'), mdiaLen);
                    if (mdia) {
                        uint32_t hdlrLen = 0;
                        uint32_t hdlr = findAtom(mdia, mdiaLen, FCC('h','d','l','r'), hdlrLen);
                        if (hdlr && hdlrLen >= 12) {
                            _f.seek(hdlr + 4); r32(_f);
                            uint32_t htype = r32(_f);
                            Serial.printf("M4A: trak hdlr=0x%08X\n", htype);
                            if (htype == FCC('s','o','u','n')) {
                                uint32_t minfLen = 0;
                                uint32_t minf = findAtom(mdia, mdiaLen, FCC('m','i','n','f'), minfLen);
                                if (minf) stblPos = findAtom(minf, minfLen, FCC('s','t','b','l'), stblLen);
                            }
                        }
                    }
                }
                if (stblPos) break;
                cur += sz;
            }
        }
        if (!stblPos) { Serial.println("M4A: no stbl"); return false; }

        // stco / co64 — cache all chunk offsets in RAM (small)
        {
            bool use64 = false;
            uint32_t plen = 0;
            uint32_t p = findAtom(stblPos, stblLen, FCC('s','t','c','o'), plen);
            if (!p) {
                p = findAtom(stblPos, stblLen, FCC('c','o','6','4'), plen);
                if (!p) { Serial.println("M4A: no stco/co64"); return false; }
                use64 = true;
            }
            _f.seek(p + 4);
            _st.chunkCount    = r32(_f);
            _st.chunkOffsets  = (uint32_t*)malloc(_st.chunkCount * sizeof(uint32_t));
            if (!_st.chunkOffsets) { Serial.println("M4A: OOM chunkOffsets"); return false; }
            for (uint32_t i = 0; i < _st.chunkCount; i++)
                _st.chunkOffsets[i] = use64 ? (uint32_t)r64(_f) : r32(_f);
            Serial.printf("M4A: chunks=%u use64=%d\n", _st.chunkCount, use64);
        }

        // stsz — store file offset only, NO malloc for sample sizes
        {
            uint32_t plen = 0;
            uint32_t p = findAtom(stblPos, stblLen, FCC('s','t','s','z'), plen);
            if (!p) { Serial.println("M4A: no stsz"); return false; }
            _f.seek(p + 4);
            _st.defaultSize = r32(_f);
            _st.sampleCount = r32(_f);
            // stsz payload: version+flags(4) + defaultSize(4) + sampleCount(4) = 12 bytes header
            // per-sample entries start at p+12
            _st.stszDataOffset = p + 12;
            Serial.printf("M4A: samples=%u defSz=%u stszOff=%u\n",
                          _st.sampleCount, _st.defaultSize, _st.stszDataOffset);
        }

        // stsc — tiny, cache in RAM
        {
            uint32_t plen = 0;
            uint32_t p = findAtom(stblPos, stblLen, FCC('s','t','s','c'), plen);
            if (!p) { Serial.println("M4A: no stsc"); return false; }
            _f.seek(p + 4);
            uint32_t n = r32(_f);
            _st.stscCount = min(n, (uint32_t)SampleTable::MAX_STSC);
            for (uint32_t i = 0; i < _st.stscCount; i++) {
                _st.stsc[i].firstChunk = r32(_f);
                _st.stsc[i].spc        = r32(_f);
                r32(_f); // skip sample_description_index
            }
            Serial.printf("M4A: stsc entries=%u\n", _st.stscCount);
        }

        // totalBytes used only for getSize() progress — estimate is fine.
        if (_st.defaultSize) {
            _st.totalBytes = (_st.defaultSize + 7) * _st.sampleCount;
        } else {
            // Sample first 16 entries to estimate average frame size
            uint32_t sum = 0, n = min(_st.sampleCount, (uint32_t)16);
            _f.seek(_st.stszDataOffset);
            for (uint32_t i = 0; i < n; i++) sum += r32(_f);
            _st.totalBytes = (uint32_t)((float)sum / n * (_st.sampleCount + 7));
        }

        // ADTS params from stsd/mp4a
        _st.adtsValid = false;
        _st.adtsSrIdx = 4;  // default 44100
        _st.adtsChans = 2;
        {
            uint32_t stsdLen = 0;
            uint32_t stsd = findAtom(stblPos, stblLen, FCC('s','t','s','d'), stsdLen);
            if (stsd) {
                uint32_t mp4aLen = 0;
                uint32_t mp4a = findAtom(stsd + 8, stsdLen - 8, FCC('m','p','4','a'), mp4aLen);
                if (mp4a) {
                    _f.seek(mp4a + 16);
                    uint16_t chans = (uint16_t)(_f.read()<<8 | _f.read());
                    _f.seek(mp4a + 24);
                    uint32_t sr = r32(_f) >> 16;
                    const uint32_t srTable[13] = {
                        96000,88200,64000,48000,44100,32000,
                        24000,22050,16000,12000,11025,8000,7350
                    };
                    uint8_t srIdx = 4;
                    for (int i = 0; i < 13; i++)
                        if (srTable[i] == sr) { srIdx = i; break; }
                    _st.adtsSrIdx = srIdx;
                    _st.adtsChans = (uint8_t)chans;
                    _st.adtsValid = true;
                    Serial.printf("M4A: esds sr=%u srIdx=%u chans=%u\n", sr, srIdx, chans);
                }
            }
        }

        return _st.sampleCount > 0 && _st.chunkCount > 0;
    }
};

// ── SD SPI pins ───────────────────────────────────────────────
#define SD_CS   12
#define SD_MOSI 14
#define SD_CLK  40
#define SD_MISO 39

// ── Screen layout constants ───────────────────────────────────
#define SCREEN_W        240
#define SCREEN_H        135
#define HEADER_H         32
#define LIST_Y           33
#define LIST_ITEM_H      14
#define VISIBLE_TRACKS    6
#define STATUS_H         17
#define STATUS_Y        (SCREEN_H - STATUS_H)

// ── RGB565 helper ─────────────────────────────────────────────
static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint16_t)(r>>3)<<11)|((uint16_t)(g>>2)<<5)|(b>>3);
}

// =====================================================================
//  Theme descriptor
// =====================================================================
struct Theme {
    uint16_t    bg;          // main background
    uint16_t    hdrBg;       // header + status background
    uint16_t    accent1;     // primary accent  (playing indicator, bar fill)
    uint16_t    accent2;     // secondary accent (cursor / selection)
    uint16_t    accent3;     // counter / volume colour
    uint16_t    textBright;  // bright text
    uint16_t    textMid;     // mid-brightness text
    uint16_t    textDim;     // dim text / hints
    uint16_t    barBg;       // progress trough
    uint16_t    selRow;      // selected-row bg tint
    const char* name;        // shown in toast on switch
};

// ── Theme 1 — NEON NOIR  (#020408 navy / #ff2d78 magenta / #00f5ff cyan)
static const Theme T_NEON = {
    rgb(  2,  4,  8),   // bg        #020408
    rgb(  5, 14, 24),   // hdrBg     #050e18
    rgb(255, 45,120),   // accent1   #ff2d78  magenta  (playing, bar fill tip)
    rgb(  0,245,255),   // accent2   #00f5ff  cyan     (selected, bar fill)
    rgb(245,230, 66),   // accent3   #f5e642  yellow   (counter, volume)
    rgb(200,234,245),   // textBright #c8eaf5
    rgb( 58,106,122),   // textMid   mid blue-grey
    rgb( 14, 48, 64),   // textDim   #0e3040  very dim
    rgb(  9, 21, 32),   // barBg     #091520
    rgb( 10, 30, 46),   // selRow    #0a1e2e  (mockup gradient start)
    "NEON NOIR"
};

// ── Theme 2 — GLITCH TERMINAL  (#000d00 / #00ff41 phosphor / #ffb300 amber)
static const Theme T_TERM = {
    rgb(  0, 13,  0),   // bg        #000d00
    rgb(  0, 13,  0),   // hdrBg     same — header has no separate bg in mockup
    rgb(  0,255, 65),   // accent1   #00ff41  phosphor green
    rgb(255,179,  0),   // accent2   #ffb300  amber
    rgb(255,179,  0),   // accent3   amber
    rgb(  0,255, 65),   // textBright full phosphor
    rgb(  0,170, 43),   // textMid   #00aa2b
    rgb(  0, 85, 18),   // textDim   #005512
    rgb(  0, 13,  0),   // barBg     same as bg (blocks sit on bg)
    rgb(  0, 22,  0),   // selRow    very subtle green tint
    "GLITCH TERMINAL"
};

// ── Theme 3 — CORPO CHROME  (#080a0f / #c8a84b gold / #c8cfe0 chrome)
static const Theme T_CORP = {
    rgb(  8, 10, 15),   // bg        #080a0f
    rgb( 12, 15, 24),   // hdrBg     #0c0f18
    rgb(200,168, 75),   // accent1   #c8a84b  gold
    rgb(232,201,106),   // accent2   #e8c96a  gold-bright (selected highlight)
    rgb(200,168, 75),   // accent3   gold
    rgb(200,207,224),   // textBright #c8cfe0  chrome
    rgb( 58, 74,106),   // textMid   #3a4a6a
    rgb( 28, 34, 53),   // textDim   #1c2235
    rgb( 28, 34, 53),   // barBg     #1c2235
    rgb( 14, 18, 30),   // selRow    subtle gold tint bg
    "CORPO CHROME"
};

// ── Theme 4 — MIAMI VICE  (#020610 dark navy / #ff3eb5 hot pink / #00e5cc turquoise)
static const Theme T_MIAMI = {
    rgb(  2,  6, 16),   // bg        very dark navy
    rgb(  5, 12, 30),   // hdrBg     deep navy
    rgb(255, 62,181),   // accent1   #ff3eb5  hot pink  (playing, bar tip)
    rgb(  0,229,204),   // accent2   #00e5cc  turquoise (selected, bar fill)
    rgb(255,210,  0),   // accent3   #ffd200  gold      (counter, volume)
    rgb(255,200,235),   // textBright pale pink-white
    rgb(120, 80,140),   // textMid   muted violet
    rgb( 40, 20, 55),   // textDim   dark purple
    rgb(  8, 10, 28),   // barBg     near-black navy
    rgb( 20, 10, 38),   // selRow    deep violet tint
    "MIAMI VICE"
};

// ── Theme 5 — ASH  (#101010 / #ffffff white / #888888 grey — no colour)
static const Theme T_ASH = {
    rgb( 16, 16, 16),   // bg        #101010  near-black
    rgb( 22, 22, 22),   // hdrBg     slightly lighter
    rgb(255,255,255),   // accent1   pure white   (playing, bar tip)
    rgb(180,180,180),   // accent2   light grey   (selected)
    rgb(140,140,140),   // accent3   mid grey     (counter, volume)
    rgb(230,230,230),   // textBright off-white
    rgb(140,140,140),   // textMid   mid grey
    rgb( 60, 60, 60),   // textDim   dark grey
    rgb( 35, 35, 35),   // barBg     dark grey trough
    rgb( 30, 30, 30),   // selRow    subtle row tint
    "ASH"
};

static const Theme* THEMES[5] = { &T_NEON, &T_TERM, &T_CORP, &T_MIAMI, &T_ASH };
static uint8_t       themeIdx  = 0;
static const Theme*  T         = &T_NEON;   // active theme

// =====================================================================
//  AudioOutputM5Speaker — bridges ESP8266Audio → M5Unified Speaker
// =====================================================================
class AudioOutputM5Speaker : public AudioOutput {
public:
    static const size_t BUF_VALS = 1024 * 2;

    AudioOutputM5Speaker(m5::Speaker_Class* spk, uint8_t ch = 0)
        : _spk(spk), _ch(ch), _wi(0), _wv(0) {
        for (int i = 0; i < 3; i++) {
            _buf[i] = new int16_t[BUF_VALS];
            memset(_buf[i], 0, BUF_VALS * sizeof(int16_t));
        }
    }
    ~AudioOutputM5Speaker() { for (int i=0;i<3;i++) delete[] _buf[i]; }

    bool begin() override { _wi = 0; _wv = 0; return true; }

    bool ConsumeSample(int16_t sample[2]) override {
        if (_wv >= BUF_VALS) { flush(); return false; }
        _buf[_wi][_wv++] = sample[0];
        _buf[_wi][_wv++] = sample[1];
        return true;
    }
    void flush() override {
        if (_wv == 0) return;
        while (!_spk->playRaw(_buf[_wi], _wv, hertz, true, 1, _ch)) taskYIELD();
        _wi = (_wi < 2) ? _wi + 1 : 0;
        _wv = 0;
    }
    bool stop()              override { flush(); _spk->stop(_ch); return true; }
    bool SetRate(int hz)     override { hertz    = hz; return true; }
    bool SetChannels(int ch) override { channels = ch; return true; }
    bool SetBitsPerSample(int)        { return true; }

private:
    m5::Speaker_Class* _spk;
    uint8_t  _ch;
    int16_t* _buf[3];
    int      _wi;
    size_t   _wv;
};

// ── Audio pipeline ────────────────────────────────────────────
AudioGeneratorMP3    *mp3     = nullptr;
AudioGeneratorAAC    *aac     = nullptr;
AudioFileSourceSD    *fileSrc = nullptr;
AudioFileSourceID3   *id3Src  = nullptr;
AudioFileSourceM4A   *m4aSrc  = nullptr;   // MP4 demuxer for iTunes M4A
AudioOutputM5Speaker *output  = nullptr;

// ── Folder tree (built once at startup by scanDir) ────────────
struct TrackEntry {
    String path;
    String label;
    size_t fileSize;
    unsigned long durationMs;  // 0 = unknown
    bool   isM4A;              // cached extension flag
};

// Lightweight name-only entry for the large-folder name cache (~50 bytes each).
// 200 entries = ~10KB. Avoids re-scanning the directory on page turns.
struct NameEntry {
    String label;
    String fullPath;
    size_t fileSize;
    bool   isM4A;
};

struct FolderEntry {
    String path;
    String label;
    std::vector<TrackEntry>  tracks;       // current page tracks with durations (PAGE_SIZE max)
    std::vector<NameEntry>   nameCache;    // sorted track names, up to NAME_CACHE_MAX entries
    std::vector<int>         subFolderIds;
    bool scanned        = false;  // true once directory listing is read from SD
    bool nameCacheReady = false;  // true once nameCache is populated (may be partial for huge folders)
    int  totalItems     = 0;      // total subfolders + tracks
    int  trackWindowPage = -1;    // page whose durations are loaded in tracks[] (-1 = none)
    int  nameCacheStart  = 0;     // track index of nameCache[0] in the full sorted list
};
std::vector<FolderEntry> allFolders;   // index 0 = /Music root
std::vector<int>         folderStack;  // navigation history
int currentFolderIdx = 0;

// ── Browser item ──────────────────────────────────────────────
struct BrowserItem {
    bool     isFolder;
    String   path;
    String   label;
    size_t   fileSize;
    unsigned long durationMs;  // 0 = unknown
};

// ── Player state ──────────────────────────────────────────────
std::vector<BrowserItem> items;       // current browser view
String        viewFolder   = "/Music"; // folder currently displayed
int           currentTrack = -1;       // index into items[] of playing track
int           selectedItem = 0;        // cursor position in items[]
bool          isPlaying    = false;
bool          isPaused     = false;
uint8_t       repeatMode   = 2;      // 0=off  1=one  2=all (default: all)
bool          shuffleOn    = false;
bool          isScanning   = false;  // true while scanFolderNow() runs — blocks input
uint8_t       volume       = 128;
bool          isRecentView = false;
int           folderPage   = 0;   // current pagination page (0-based) for large folders

#define RECENT_MAX     10
#define SCAN_CACHE_MAX       11   // max simultaneously-scanned folders; LRU eviction beyond this
#define PAGE_SIZE            25   // max items shown per page; BACK/MORE injected when exceeded
#define NAME_CACHE_MAX      200   // max track NameEntries kept per large folder (~10KB)

String        recentPaths[RECENT_MAX];
int           recentCount = 0;

// LRU scan cache — stores allFolders indices in access order (front = most recent)
int           scanLRU[SCAN_CACHE_MAX];
int           scanLRUCount = 0;
unsigned long trackStartMs    = 0;
unsigned long pausedElapsedMs = 0;
unsigned long trackDurationMs = 0;
int           batteryLevel    = -1;       // -1 = not yet read
unsigned long batteryLastMs   = 0;        // millis() of last battery read
#define BATTERY_INTERVAL (2UL * 60 * 1000) // 2 minutes

// ── Sprites ───────────────────────────────────────────────────
M5Canvas statusCanvas(&M5Cardputer.Display);
M5Canvas headerCanvas(&M5Cardputer.Display);
// (help overlay draws directly to display — no sprite needed)
bool          toastActive  = false;
unsigned long toastEnd     = 0;
char          hdrMsg[20]   = "";     // short message shown below counter in header
unsigned long hdrMsgEnd    = 0;      // millis() when hdrMsg clears (0 = inactive)
bool          helpVisible  = false;
bool          screenOn     = true;   // tracks display on/off state
bool          cursorVisible = true;  // terminal cursor blink state (toggled every 500ms)
bool          settingsDirty = false; // true when theme/volume/repeat/shuffle need saving
unsigned long settingsDirtyMs = 0;   // millis() when settings were last changed

// ── Forward declarations ──────────────────────────────────────
void drawSplash(const char* statusLine);
void loadFolder(const String& path);
void loadFolderIdx(int idx);
void scanFolderNow(int idx);
int  scanDir(const String& path, const String& label);
unsigned long readM4ADuration(const char* path);
unsigned long readMP3Duration(const char* path, size_t fileSize);
void addRecent(const String& path);
void loadRecentView();
void saveRecentToSD();
void loadRecentFromSD();
void saveSettings();
void loadSettings();
void enterItem(int idx);
void startTrack(int idx);
void stopAudio();
void pauseAudio();
void resumeAudio();
void pumpAudio();
void setTheme(uint8_t idx);
void toggleScreen();
void goBack();
void drawAll();
void drawHeader();
void drawTrackList();
void drawStatus();
void showToast();
void showHdrMsg(const char* msg);
void cycleRepeat();
void toggleShuffle();
void toggleHelp();
void drawHelp();
String shortName(const String &p, int maxCh);
String folderName(const String &p, int maxCh);
String formatTime(unsigned long ms);
unsigned long estimateDuration(int idx);

// =============================================================
//  drawSplash  — cyberpunk boot screen for BROKEN SIGNAL
// =============================================================
void drawSplash(const char* statusLine) {
    uint16_t bg      = rgb(  2,  4,  8);
    uint16_t magenta = rgb(255, 45,120);
    uint16_t cyan    = rgb(  0,245,255);
    uint16_t yellow  = rgb(245,230, 66);
    uint16_t dimBlue = rgb( 14, 48, 64);
    uint16_t midBlue = rgb( 20, 55, 75);
    uint16_t red     = rgb(255, 30, 50);

    M5Cardputer.Display.fillScreen(bg);

    // ── Diagonal grid lines ───────────────────────────────────
    for (int i = -SCREEN_H; i < SCREEN_W; i += 18) {
        int x0 = i,          y0 = 0;
        int x1 = i+SCREEN_H, y1 = SCREEN_H;
        if (x0 < 0) { y0 -= x0; x0 = 0; }
        if (x1 >= SCREEN_W) { y1 -= (x1-(SCREEN_W-1)); x1 = SCREEN_W-1; }
        M5Cardputer.Display.drawLine(x0, y0, x1, y1, dimBlue);
    }

    // ── Horizontal scan bands ─────────────────────────────────
    for (int y = 0; y < SCREEN_H; y += 4)
        M5Cardputer.Display.drawFastHLine(0, y, SCREEN_W, midBlue);

    // ── Glitch displacement bars — broken signal aesthetic ────
    // Simulate horizontal data corruption slices
    M5Cardputer.Display.fillRect(0,  22, 240, 2, midBlue);
    M5Cardputer.Display.fillRect(0,  23, 120, 2, dimBlue);   // half-width offset
    M5Cardputer.Display.fillRect(18, 44, 80,  1, magenta);
    M5Cardputer.Display.fillRect(140, 44, 60, 1, magenta);
    M5Cardputer.Display.fillRect(0,  88, 240, 2, midBlue);
    M5Cardputer.Display.fillRect(60, 89, 180, 1, dimBlue);   // offset slice
    M5Cardputer.Display.fillRect(10,105, 50,  1, red);       // corrupted red fragment
    M5Cardputer.Display.fillRect(180,105, 40, 1, red);

    // ── Outer border ──────────────────────────────────────────
    M5Cardputer.Display.drawRect(0, 0, SCREEN_W, SCREEN_H, magenta);
    M5Cardputer.Display.drawRect(2, 2, SCREEN_W-4, SCREEN_H-4, dimBlue);

    // ── Corner brackets ───────────────────────────────────────
    M5Cardputer.Display.drawFastHLine(0,          0,          10, cyan);
    M5Cardputer.Display.drawFastVLine(0,          0,          10, cyan);
    M5Cardputer.Display.drawFastHLine(SCREEN_W-10,0,          10, cyan);
    M5Cardputer.Display.drawFastVLine(SCREEN_W-1, 0,          10, cyan);
    M5Cardputer.Display.drawFastHLine(0,          SCREEN_H-1, 10, cyan);
    M5Cardputer.Display.drawFastVLine(0,          SCREEN_H-10,10, cyan);
    M5Cardputer.Display.drawFastHLine(SCREEN_W-10,SCREEN_H-1, 10, cyan);
    M5Cardputer.Display.drawFastVLine(SCREEN_W-1, SCREEN_H-10,10, cyan);

    // ── Top status bar ────────────────────────────────────────
    M5Cardputer.Display.setTextDatum(middle_left);
    M5Cardputer.Display.setTextColor(dimBlue);
    M5Cardputer.Display.drawString("// SYS:BOOT //", 8, 10, 1);
    M5Cardputer.Display.setTextDatum(middle_right);
    M5Cardputer.Display.setTextColor(red);
    M5Cardputer.Display.drawString("ERR_SIG", SCREEN_W-8, 10, 1);

    // ── Main title: BROKEN / SIGNAL stacked ──────────────────
    // "BROKEN" — chromatic aberration in red/magenta (corrupted)
    int titleY1 = 52;   // top word baseline
    int titleY2 = 78;   // bottom word baseline
    M5Cardputer.Display.setTextDatum(middle_center);

    // BROKEN — offset yellow ghost + magenta ghost + warm white on top
    M5Cardputer.Display.setTextColor(yellow);
    M5Cardputer.Display.drawString("BROKEN", SCREEN_W/2 - 2, titleY1, 4);
    M5Cardputer.Display.setTextColor(magenta);
    M5Cardputer.Display.drawString("BROKEN", SCREEN_W/2 + 1, titleY1, 4);
    M5Cardputer.Display.setTextColor(rgb(255,240,200));  // warm white
    M5Cardputer.Display.drawString("BROKEN", SCREEN_W/2,     titleY1, 4);

    // SIGNAL — chromatic aberration in cyan/magenta (glowing)
    M5Cardputer.Display.setTextColor(magenta);
    M5Cardputer.Display.drawString("SIGNAL", SCREEN_W/2 - 1, titleY2, 4);
    M5Cardputer.Display.setTextColor(cyan);
    M5Cardputer.Display.drawString("SIGNAL", SCREEN_W/2 + 1, titleY2, 4);
    M5Cardputer.Display.setTextColor(rgb(200,234,245));  // cool white
    M5Cardputer.Display.drawString("SIGNAL", SCREEN_W/2,     titleY2, 4);

    // ── Status line (bottom) ──────────────────────────────────
    M5Cardputer.Display.setTextColor(yellow);
    M5Cardputer.Display.drawString(statusLine, SCREEN_W/2, SCREEN_H - 10, 1);
}

// =============================================================
void setup() {
    Serial.begin(115200);
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Speaker.begin();
    M5Cardputer.Speaker.setVolume(volume);
    M5Cardputer.Display.setRotation(1);

    drawSplash("> SCANNING /MUSIC/...");

    SPI.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS)) {
        drawSplash("> SD CARD ERROR — REBOOT");
        M5Cardputer.Display.setTextDatum(middle_center);
        M5Cardputer.Display.setTextColor(rgb(255,45,120));
        M5Cardputer.Display.drawString("INSERT CARD", SCREEN_W/2, SCREEN_H/2 + 32, 1);
        while (true) delay(1000);
    }

    output = new AudioOutputM5Speaker(&M5Cardputer.Speaker, 0);
    statusCanvas.createSprite(SCREEN_W, STATUS_H);
    headerCanvas.createSprite(SCREEN_W, HEADER_H);
    // Help overlay draws directly to display (zero heap cost).
    // a large contiguous block free for the AAC SBR decoder at play time.

    allFolders.clear();
    scanDir("/Music", "Music");
    loadRecentFromSD();
    loadSettings();
    loadFolderIdx(0);

    // Count total tracks found across all folders
    // Only root is scanned at startup; subfolders are lazy.
    // Check root has something — subfolders count as browsable even if empty root.
    int totalTracks = (int)allFolders[0].tracks.size() + (int)allFolders[0].subFolderIds.size();
    if (totalTracks == 0) {
        drawSplash("> NO FILES IN /MUSIC/");
        while (true) delay(1000);
    }

    delay(300);
    drawAll();
}

// =============================================================
void loop() {
    M5Cardputer.update();

    // Audio must be pumped every iteration — highest priority
    pumpAudio();
    // Yield after pump: lets RTOS speaker-DMA task run, reduces CPU burn.
    // 1ms while playing (safe — 69ms triple-buffer headroom at 44100Hz).
    // 10ms while idle — cuts CPU ~50x with no perceptible input latency.
    delay(isPlaying ? 1 : 10);

    // Keyboard
    if (!isScanning && M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        Keyboard_Class::KeysState ks = M5Cardputer.Keyboard.keysState();

        // While help is open, only H closes it — ignore everything else
        if (helpVisible) {
            for (auto c : ks.word)
                if (c == 'h' || c == 'H') toggleHelp();
            return;
        }

        if (ks.enter) enterItem(selectedItem);
        if (ks.del)   goBack();

        for (auto c : ks.word) {
            switch (c) {
                // ── Navigation ──────────────────────────────
                case ';':
                    selectedItem = (selectedItem - 1 + (int)items.size()) % (int)items.size();
                    drawTrackList();
                    break;
                case '.':
                    selectedItem = (selectedItem + 1) % (int)items.size();
                    drawTrackList();
                    break;
                case ',':
                    // Prev: previous track while playing/paused;
                    // previous page while browsing a paged folder;
                    // back to parent folder when on page 0
                    if (isPlaying || isPaused) {
                        if (currentTrack > 0) {
                            int p = currentTrack - 1;
                            while (p >= 0 && items[p].isFolder) p--;
                            if (p >= 0) startTrack(p);
                        }
                    } else if (folderPage > 0) {
                        stopAudio();
                        folderPage--;
                        loadFolderIdx(currentFolderIdx);
                        selectedItem = 0;
                        drawAll();
                    } else {
                        goBack();
                    }
                    break;
                case '/':
                    // Next: next track while playing/paused; next page while browsing
                    if (isPlaying || isPaused) {
                        int n = pickNextTrack();
                        if (n >= 0) startTrack(n);
                    } else {
                        int total = allFolders[currentFolderIdx].totalItems;
                        if ((folderPage + 1) * PAGE_SIZE < total) {
                            stopAudio();
                            folderPage++;
                            loadFolderIdx(currentFolderIdx);
                            selectedItem = 0;
                            drawAll();
                        }
                    }
                    break;
                // ── Playback ────────────────────────────────
                case ' ':
                    if      (isPlaying) pauseAudio();
                    else if (isPaused)  resumeAudio();
                    else {
                        // play first non-folder item or current
                        int t = (currentTrack >= 0) ? currentTrack : -1;
                        if (t < 0) {
                            for (int i = 0; i < (int)items.size(); i++)
                                if (!items[i].isFolder) { t = i; break; }
                        }
                        if (t >= 0) startTrack(t);
                    }
                    break;
                case '+': case '=':
                    volume = (uint8_t)min(255, (int)volume + 10);
                    M5Cardputer.Speaker.setVolume(volume);
                    settingsDirty = true; settingsDirtyMs = millis();
                    drawStatus();
                    break;
                case '-':
                    volume = (uint8_t)max(0, (int)volume - 10);
                    M5Cardputer.Speaker.setVolume(volume);
                    settingsDirty = true; settingsDirtyMs = millis();
                    drawStatus();
                    break;
                // ── Theme select ─────────────────────────────
                case '1': setTheme(0); break;
                case '2': setTheme(1); break;
                case '3': setTheme(2); break;
                case '4': setTheme(3); break;
                case '5': setTheme(4); break;
                // ── Repeat mode ──────────────────────────────
                case 'r': case 'R': cycleRepeat(); break;
                // ── Shuffle ──────────────────────────────────
                case 's': case 'S': toggleShuffle(); break;
                // ── Screen on/off ────────────────────────────
                case 'o': case 'O': toggleScreen(); break;
                // ── Help overlay ─────────────────────────────
                case 'h': case 'H': toggleHelp(); break;
            }
        }
    }

    // 500 ms status bar refresh — skip while help overlay is covering the screen
    static unsigned long lastDraw = 0;
    if (!helpVisible && millis() - lastDraw >= 500) {
        lastDraw = millis();
        cursorVisible = !cursorVisible;
        // Redraw header when blinking elements are active:
        // Terminal: cursor blinks only while playing or a track is selected
        // Others: only while playing (state icon blinks)
        bool terminalNeedsBlink = (themeIdx == 1) &&
            (isPlaying || isPaused ||
             (selectedItem >= 0 && selectedItem < (int)items.size() &&
              !items[selectedItem].isFolder));
        bool headerNeedsBlink = terminalNeedsBlink || isPlaying;
        if (headerNeedsBlink) drawHeader();
        // Status bar only needs updating while playing (elapsed time moves).
        // Paused/stopped: bar is static, skip the redraw to save CPU.
        if (isPlaying) drawStatus();
    }

    // Toast expiry — repaint only if help is not covering the screen
    if (toastActive && millis() > toastEnd) {
        toastActive = false;
        if (!helpVisible) drawAll();
    }

    // Header message expiry — redraw header to clear it
    if (hdrMsgEnd > 0 && millis() >= hdrMsgEnd) {
        hdrMsgEnd = 0;
        if (!helpVisible) drawHeader();
    }

    // Deferred settings save — write only when not playing and 2s after last change
    if (settingsDirty && !isPlaying && !isPaused &&
        millis() - settingsDirtyMs >= 2000) {
        saveSettings();
        settingsDirty = false;
    }

    // Battery level — read once at startup (batteryLevel == -1) then every 2 minutes.
    // AXP2101 is on its own I2C bus, safe to call during M4A playback (~0.1ms).
    if (batteryLevel < 0 || millis() - batteryLastMs >= BATTERY_INTERVAL) {
        batteryLevel  = (int)min((int32_t)99, M5.Power.getBatteryLevel());
        batteryLastMs = millis();
        if (!helpVisible) drawStatus();
    }
}

// =============================================================
void pumpAudio() {
    if (!isPlaying) return;
    AudioGenerator* gen = mp3 ? (AudioGenerator*)mp3
                        : aac ? (AudioGenerator*)aac
                        : nullptr;
    if (!gen) return;
    if (!gen->isRunning()) {
        static bool warnedNotRunning = false;
        if (!warnedNotRunning) {
            Serial.println("AUDIO: generator not running");
            warnedNotRunning = true;
        }
        return;
    }
    if (!gen->loop()) {
        stopAudio();
        int next = (repeatMode == 1) ? currentTrack : pickNextTrack();
        if (next >= 0) {
            startTrack(next);
        } else {
            currentTrack = -1;
            drawAll();
        }
    }
}

// =============================================================
// =============================================================
//  readM4ADuration — reads exact duration from the mvhd atom.
//  mvhd is a child of moov and contains timescale + duration fields.
//  Cost: 2 seeks + ~40 bytes read. Returns 0 on failure.
// =============================================================
unsigned long readM4ADuration(const char* path) {
    File f = SD.open(path);
    if (!f) return 0;

    uint32_t fsz = f.size();

    auto r32 = [&]() -> uint32_t {
        uint8_t b[4]; f.read(b, 4);
        return ((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|b[3];
    };
    auto fcc = [](char a,char b,char c,char d) -> uint32_t {
        return ((uint32_t)(uint8_t)a<<24)|((uint32_t)(uint8_t)b<<16)
              |((uint32_t)(uint8_t)c<<8)|(uint8_t)d;
    };

    uint32_t moovTag  = fcc('m','o','o','v');
    uint32_t moovStart = 0, moovLen = 0;

    // ── Check for moov at end of file first ────────────────────────────
    // Most encoders (iTunes, ffmpeg default) place moov AFTER mdat.
    // Reading the last 8 bytes costs one cheap near-end seek with no FAT
    // chain walk, avoiding the 1-2 second penalty of seeking past a large mdat.
    if (fsz > 8) {
        f.seek(fsz - 8);
        uint32_t lastSz  = r32();
        uint32_t lastTag = r32();
        if (lastTag == moovTag && lastSz >= 8 && lastSz <= fsz) {
            moovStart = fsz - lastSz + 8;
            moovLen   = lastSz - 8;
        }
    }

    // ── Fallback: forward scan (fast-start files with moov before mdat) ─
    if (!moovLen) {
        uint32_t pos = 0;
        while (pos + 8 <= fsz) {
            f.seek(pos);
            uint32_t sz  = r32();
            uint32_t tag = r32();
            if (sz < 8) break;
            if (tag == moovTag) { moovStart = pos + 8; moovLen = sz - 8; break; }
            pos += sz;
        }
    }

    if (!moovLen) { f.close(); return 0; }

    // ── Find mvhd inside moov ───────────────────────────────────────────
    uint32_t pos = moovStart;
    uint32_t end = moovStart + moovLen;
    while (pos + 8 <= end) {
        f.seek(pos);
        uint32_t sz  = r32();
        uint32_t tag = r32();
        if (sz < 8) break;
        if (tag == fcc('m','v','h','d')) {
            f.seek(pos + 8);
            uint8_t version = 0; f.read(&version, 1);
            if (version == 0) {
                f.seek(pos + 8 + 1 + 3 + 4 + 4);
                uint32_t timescale = r32();
                uint32_t duration  = r32();
                f.close();
                if (timescale == 0) return 0;
                return (unsigned long)((uint64_t)duration * 1000 / timescale);
            } else {
                f.seek(pos + 8 + 1 + 3 + 8 + 8);
                uint32_t timescale = r32();
                uint64_t hi = r32(); uint64_t lo = r32();
                uint64_t duration = (hi << 32) | lo;
                f.close();
                if (timescale == 0) return 0;
                return (unsigned long)(duration * 1000 / timescale);
            }
        }
        pos += sz;
    }
    f.close();
    return 0;
}

unsigned long readMP3Duration(const char* path, size_t fileSize) {
    File f = SD.open(path);
    if (!f) return 0;

    // Skip ID3v2 tag if present (starts with "ID3")
    uint32_t audioStart = 0;
    uint8_t hdr[10];
    f.read(hdr, 10);
    if (hdr[0]=='I' && hdr[1]=='D' && hdr[2]=='3') {
        // ID3v2 size is encoded as 4 syncsafe bytes (7 bits each)
        uint32_t id3Size = ((uint32_t)(hdr[6] & 0x7F) << 21)
                         | ((uint32_t)(hdr[7] & 0x7F) << 14)
                         | ((uint32_t)(hdr[8] & 0x7F) <<  7)
                         |  (uint32_t)(hdr[9] & 0x7F);
        audioStart = 10 + id3Size;
    }

    // Read first MPEG sync word
    f.seek(audioStart);
    uint8_t b[4]; f.read(b, 4);
    f.close();

    // Validate sync: 0xFFE or 0xFFF
    if (b[0] != 0xFF || (b[1] & 0xE0) != 0xE0) return 0;

    // Bitrate index tables (kbps) for MPEG1 Layer3 and MPEG2 Layer3
    static const uint16_t br1[16] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
    static const uint16_t br2[16] = {0, 8,16,24,32,40,48,56, 64, 80, 96,112,128,144,160,0};

    uint8_t  version  = (b[1] >> 3) & 0x3;  // 3=MPEG1, 2=MPEG2, 0=MPEG2.5
    uint8_t  layer    = (b[1] >> 1) & 0x3;  // 1=Layer3
    uint8_t  brIdx    = (b[2] >> 4) & 0xF;

    if (layer != 1) return 0;  // only Layer 3 (MP3)
    uint16_t bitrateKbps = (version == 3) ? br1[brIdx] : br2[brIdx];
    if (bitrateKbps == 0) return 0;

    uint32_t audioBytes = fileSize - audioStart;
    return (unsigned long)((uint64_t)audioBytes * 8 / ((uint32_t)bitrateKbps * 1000) * 1000);
}

// Move idx to front of LRU array (most-recently-used).
static void lruMoveToFront(int idx) {
    for (int i = 0; i < scanLRUCount; i++) {
        if (scanLRU[i] == idx) {
            for (int j = i; j > 0; j--) scanLRU[j] = scanLRU[j-1];
            scanLRU[0] = idx;
            return;
        }
    }
}

// =============================================================
//  scanFolderNow — two-tier scan:
//  Tier 1: read all names into nameCache[] sorted (fast, SD names only).
//  Tier 2: read durations for current page only (slow, 25 files max).
//  Page turns skip Tier 1 — nameCache persists across pages.
// =============================================================
void scanFolderNow(int idx) {
    if (idx < 0 || idx >= (int)allFolders.size()) return;
    FolderEntry& fe = allFolders[idx];

    // If nameCache is ready, only Tier 2 (duration read) needs to run.
    bool needsDirectoryScan = !fe.nameCacheReady;

    if (fe.scanned && fe.trackWindowPage == folderPage) {
        lruMoveToFront(idx);
        return;
    }

    if (needsDirectoryScan) {
        // First time seeing this folder — evict LRU if cache full
        if (scanLRUCount >= SCAN_CACHE_MAX) {
            for (int i = scanLRUCount - 1; i >= 0; i--) {
                int evict = scanLRU[i];
                if (evict == 0 || evict == currentFolderIdx) continue;
                allFolders[evict].tracks.clear();
                allFolders[evict].tracks.shrink_to_fit();
                allFolders[evict].nameCache.clear();
                allFolders[evict].nameCache.shrink_to_fit();
                allFolders[evict].scanned         = false;
                allFolders[evict].nameCacheReady  = false;
                allFolders[evict].nameCacheStart  = 0;
                allFolders[evict].totalItems      = 0;
                allFolders[evict].trackWindowPage = -1;
                for (int j = i; j < scanLRUCount - 1; j++) scanLRU[j] = scanLRU[j+1];
                scanLRUCount--;
                break;
            }
        }
        // Add to front of LRU
        for (int j = min(scanLRUCount, SCAN_CACHE_MAX - 1); j > 0; j--)
            scanLRU[j] = scanLRU[j-1];
        scanLRU[0] = idx;
        if (scanLRUCount < SCAN_CACHE_MAX) scanLRUCount++;
    } else {
        lruMoveToFront(idx);
    }

    // ── Tier 1: directory scan (skipped if nameCache already ready) ──────
    std::vector<String> subDirs, subDirNames;
    int totalTrackCount = 0;  // set during Tier 1, used in Tier 2 for totalItems

    if (needsDirectoryScan) {
        fe.nameCache.clear();

        File dir = SD.open(fe.path.c_str());
        if (!dir || !dir.isDirectory()) return;

        int cacheStartTrack  = folderPage * PAGE_SIZE;  // fill cache from this track index

        File f;
        while ((f = dir.openNextFile())) {
            String raw = f.name();
            String nm  = raw;
            int sl = raw.lastIndexOf('/');
            if (sl >= 0) nm = raw.substring(sl + 1);
            if (nm.isEmpty() || nm.startsWith(".")) { f.close(); continue; }
            String lo = nm; lo.toLowerCase();
            String fullPath = fe.path + "/" + nm;

            if (f.isDirectory()) {
                subDirs.push_back(fullPath);
                subDirNames.push_back(nm);
            } else if (lo.endsWith(".mp3") || lo.endsWith(".m4a")) {
                totalTrackCount++;
                NameEntry ne;
                ne.fullPath = fullPath;
                ne.fileSize = f.size();
                ne.isM4A    = lo.endsWith(".m4a");
                int dot = nm.lastIndexOf('.');
                ne.label = (dot > 0) ? nm.substring(0, dot) : nm;
                ne.label.replace('_', ' ');
                // Always insert sorted — we slice to window after the loop
                auto pos = std::lower_bound(fe.nameCache.begin(), fe.nameCache.end(), ne,
                    [](const NameEntry& a, const NameEntry& b){ return a.label < b.label; });
                fe.nameCache.insert(pos, ne);
                // Keep at most cacheStartTrack + NAME_CACHE_MAX entries to bound memory.
                // Anything beyond that window can be dropped (largest labels go first).
                if ((int)fe.nameCache.size() > cacheStartTrack + NAME_CACHE_MAX)
                    fe.nameCache.pop_back();
            }
            f.close();
        }
        dir.close();

        // Trim the front of nameCache to the window starting at cacheStartTrack.
        // The sorted vector holds entries 0..N; we want [cacheStartTrack .. cacheStartTrack+NAME_CACHE_MAX).
        if (cacheStartTrack > 0 && cacheStartTrack < (int)fe.nameCache.size())
            fe.nameCache.erase(fe.nameCache.begin(), fe.nameCache.begin() + cacheStartTrack);
        if ((int)fe.nameCache.size() > NAME_CACHE_MAX)
            fe.nameCache.resize(NAME_CACHE_MAX);
        allFolders[idx].nameCacheStart = cacheStartTrack;

        // Sort subfolders
        {
            std::vector<std::pair<String,String>> pairs;
            for (int i = 0; i < (int)subDirs.size(); i++)
                pairs.push_back({subDirNames[i], subDirs[i]});
            std::sort(pairs.begin(), pairs.end(),
                [](const std::pair<String,String>& a, const std::pair<String,String>& b){
                    return a.first < b.first; });
            for (int i = 0; i < (int)pairs.size(); i++) {
                subDirNames[i] = pairs[i].first;
                subDirs[i]     = pairs[i].second;
            }
        }

        // totalItems: subfolders + all tracks (nameCache may be capped so use a
        // separate counter). Re-count by re-scanning names only if capped — but
        // for simplicity, if nameCache hit the cap we know total tracks >= NAME_CACHE_MAX.
        // We'll store subfolders + nameCache.size() as a lower bound, flagged as partial.
        // For pagination math we need the real total — count during the same pass above.
        // (We already counted via nameCache insertions; recover from totalTrackCount below.)
        // Register subfolders
        allFolders[idx].subFolderIds.clear();
        for (int i = 0; i < (int)subDirs.size(); i++) {
            int subIdx = -1;
            for (int j = 0; j < (int)allFolders.size(); j++) {
                if (allFolders[j].path == subDirs[i]) { subIdx = j; break; }
            }
            if (subIdx < 0) {
                FolderEntry stub;
                stub.path    = subDirs[i];
                stub.label   = subDirNames[i];
                stub.scanned = false;
                subIdx = (int)allFolders.size();
                allFolders.push_back(stub);
            }
            allFolders[idx].subFolderIds.push_back(subIdx);
        }

        fe.nameCacheReady = true;
        // totalItems set below after we know track count
    }

    // ── Tier 2: load durations for current page ───────────────────────────
    int numSubs         = (int)allFolders[idx].subFolderIds.size();
    int numCachedTracks = (int)allFolders[idx].nameCache.size();

    if (needsDirectoryScan) {
        allFolders[idx].totalItems = numSubs + totalTrackCount;
    }
    int total = allFolders[idx].totalItems;

    // Compute track slice for current page
    int winStart = folderPage * PAGE_SIZE;
    int tStart   = max(0, winStart - numSubs) - allFolders[idx].nameCacheStart;
    // tEnd capped at numCachedTracks — pages beyond the cache show what's available.
    // MORE still appears correctly because total reflects the real file count.
    int tEnd     = min(tStart + PAGE_SIZE, numCachedTracks);

    // Load durations for page tracks only
    allFolders[idx].tracks.clear();
    for (int i = tStart; i < tEnd; i++) {
        const NameEntry& ne = allFolders[idx].nameCache[i];
        TrackEntry te;
        te.path       = ne.fullPath;
        te.fileSize   = ne.fileSize;
        te.isM4A      = ne.isM4A;
        te.label      = ne.label;
        te.durationMs = ne.isM4A
            ? readM4ADuration(ne.fullPath.c_str())
            : readMP3Duration(ne.fullPath.c_str(), ne.fileSize);
        allFolders[idx].tracks.push_back(te);
    }

    allFolders[idx].trackWindowPage = folderPage;
    allFolders[idx].scanned         = true;
}


// Startup entry point — registers /Music root and scans only that level.
// All subfolders are stubs scanned on first navigation.
int scanDir(const String& path, const String& label) {
    FolderEntry fe;
    fe.path    = path;
    fe.label   = label;
    fe.scanned = false;
    int myIdx  = (int)allFolders.size();
    allFolders.push_back(fe);
    scanFolderNow(myIdx);
    return myIdx;
}

void loadFolderIdx(int idx) {
    if (idx < 0 || idx >= (int)allFolders.size()) return;

    // Show a subtle scanning indicator in the header, styled per theme.
    // Draw scanning indicator directly below the counter (top-right), right-aligned.
    // Same column as the counter, at the track name row — consistent across all themes.
    // Scan needed if: folder not yet seen, OR durations for folderPage not loaded yet.
    // nameCache (names only) is preserved across page turns — no directory re-scan needed.
    // Check if current page's tracks fall beyond the nameCache window.
    // If so, nameCache needs to be refilled starting from this page's offset.
    int  nSubs_   = (int)allFolders[idx].subFolderIds.size();
    int  tStart_  = max(0, folderPage * PAGE_SIZE - nSubs_);
    bool cacheMiss = allFolders[idx].nameCacheReady &&
                     tStart_ >= (int)allFolders[idx].nameCache.size();
    if (cacheMiss) {
        // Invalidate nameCache so Tier 1 re-scans with updated window offset.
        // Store the target page so Tier 1 knows where to start caching.
        allFolders[idx].nameCacheReady = false;
        allFolders[idx].scanned        = false;
    }
    bool needsScan = !allFolders[idx].nameCacheReady ||
                     allFolders[idx].trackWindowPage != folderPage;
    if (needsScan) {
        hdrMsgEnd = 0;
        hdrMsg[0] = '\0';
        M5Cardputer.Display.fillRect(SCREEN_W/2, 15, SCREEN_W/2, 14, T->hdrBg);
        M5Cardputer.Display.setTextDatum(middle_right);
        M5Cardputer.Display.setTextColor(T->accent2);
        M5Cardputer.Display.drawString("SCAN...", SCREEN_W-4, 22, 1);
    }

    isScanning = true;
    scanFolderNow(idx);
    isScanning = false;
    // Flush any keys pressed during the scan so they don't fire after load
    M5Cardputer.update();
    currentFolderIdx = idx;
    viewFolder       = allFolders[idx].path;
    currentTrack     = -1;
    selectedItem     = 0;
    isRecentView     = false;
    items.clear();

    // Inject RECENT virtual folder at top of root view only
    if (idx == 0 && recentCount > 0) {
        BrowserItem ri;
        ri.isFolder   = true;
        ri.path       = "__RECENT__";
        ri.label      = "RECENT";
        ri.fileSize   = 0;
        ri.durationMs = 0;
        items.push_back(ri);
    }

    const FolderEntry& fe = allFolders[idx];

    int  total     = fe.totalItems;
    int  numSubs   = (int)fe.subFolderIds.size();
    // fe.tracks[0] corresponds to track index (folderPage*PAGE_SIZE - numSubs),
    // adjusted for nameCacheStart offset.
    int  tStart    = max(0, folderPage * PAGE_SIZE - numSubs) - fe.nameCacheStart;

    int pageStart = folderPage * PAGE_SIZE;
    int pageEnd   = min(pageStart + PAGE_SIZE, total);

    // BACK entry if not on first page
    if (pageStart > 0) {
        BrowserItem back;
        back.isFolder   = true;
        back.path       = "__PREV__";
        back.label      = "< BACK";
        back.fileSize   = 0;
        back.durationMs = 0;
        items.push_back(back);
    }

    for (int flatIdx = pageStart; flatIdx < pageEnd; flatIdx++) {
        BrowserItem bi;
        if (flatIdx < numSubs) {
            // Subfolder
            const FolderEntry& sub = allFolders[fe.subFolderIds[flatIdx]];
            bi.isFolder   = true;
            bi.path       = sub.path;
            bi.label      = sub.label;
            bi.fileSize   = 0;
            bi.durationMs = 0;
        } else {
            // Track — fe.tracks[0] corresponds to track index tStart
            int ti = flatIdx - numSubs - tStart;
            if (ti < 0 || ti >= (int)fe.tracks.size()) continue;
            const TrackEntry& te = fe.tracks[ti];
            bi.isFolder   = false;
            bi.path       = te.path;
            bi.label      = te.label;
            bi.fileSize   = te.fileSize;
            bi.durationMs = te.durationMs;
        }
        items.push_back(bi);
    }

    // MORE entry if items remain beyond this page
    if (pageEnd < total) {
        BrowserItem more;
        more.isFolder   = true;
        more.path       = "__MORE__";
        more.label      = "MORE >";
        more.fileSize   = 0;
        more.durationMs = 0;
        items.push_back(more);
    }
}

void loadFolder(const String& path) {
    // Find folder by path in allFolders
    for (int i = 0; i < (int)allFolders.size(); i++) {
        if (allFolders[i].path == path) { loadFolderIdx(i); return; }
    }
    // Fallback to root
    loadFolderIdx(0);
}

// =============================================================
//  enterItem — called on ENTER key
// =============================================================
void enterItem(int idx) {
    if (idx < 0 || idx >= (int)items.size()) return;
    if (items[idx].isFolder) {
        if (items[idx].path == "__RECENT__") {
            stopAudio();
            folderStack.push_back(currentFolderIdx);
            loadRecentView();
            drawAll();
            return;
        }
        if (items[idx].path == "__PREV__") {
            stopAudio();
            folderPage--;
            loadFolderIdx(currentFolderIdx);
            selectedItem = (int)items.size() - 1;  // land at MORE
            drawAll();
            return;
        }
        if (items[idx].path == "__MORE__") {
            stopAudio();
            folderPage++;
            loadFolderIdx(currentFolderIdx);
            selectedItem = 0;  // land at BACK (or first track if no BACK)
            drawAll();
            return;
        }
        stopAudio();
        for (int i = 0; i < (int)allFolders.size(); i++) {
            if (allFolders[i].path == items[idx].path) {
                folderStack.push_back(currentFolderIdx);
                folderPage = 0;
                loadFolderIdx(i);
                drawAll();
                return;
            }
        }
    } else {
        if ((isPlaying || isPaused) && idx == currentTrack)
            stopAudio();
        else
            startTrack(idx);
    }
}

// =============================================================
//  goBack — called on DEL key, navigates up one folder level
// =============================================================
void goBack() {
    if (folderStack.empty()) return;
    int parentIdx = folderStack.back();
    folderStack.pop_back();
    stopAudio();
    isRecentView = false;
    folderPage   = 0;
    loadFolderIdx(parentIdx);
    drawAll();
}

unsigned long estimateDuration(int idx) {
    if (idx < 0 || idx >= (int)items.size() || items[idx].isFolder)
        return 3UL*60*1000;
    // Use header-derived duration if available
    if (items[idx].durationMs > 0)
        return items[idx].durationMs;
    // Fallback: assume ~192kbps average (reasonable for mixed libraries)
    unsigned long dur = (unsigned long)(
        ((float)items[idx].fileSize * 8.0f) / (192000.0f / 1000.0f));
    return max(5000UL, dur);
}

// =============================================================
void startTrack(int idx) {
    stopAudio();
    // Dismiss help if open when a track starts
    if (helpVisible) {
        helpVisible = false;
    }
    if (idx < 0 || idx >= (int)items.size() || items[idx].isFolder) return;
    currentTrack    = idx;
    selectedItem    = idx;
    trackStartMs    = millis();
    pausedElapsedMs = 0;
    isPaused        = false;
    trackDurationMs = estimateDuration(idx);

    String path = items[idx].path;
    String lo   = path; lo.toLowerCase();
    output->begin();

    if (lo.endsWith(".m4a")) {
        // AAC SBR decoder needs ~50KB contiguous. Evict LRU scanned folders
        // (skipping root and current) until free heap is above 80KB threshold.
        const uint32_t HEAP_THRESHOLD = 80 * 1024;
        int evictPass = 0;
        while (ESP.getFreeHeap() < HEAP_THRESHOLD && evictPass < scanLRUCount) {
            // Find LRU entry that is not root and not current folder
            for (int i = scanLRUCount - 1; i >= 0; i--) {
                int evict = scanLRU[i];
                if (evict == 0 || evict == currentFolderIdx) continue;
                allFolders[evict].tracks.clear();
                allFolders[evict].tracks.shrink_to_fit();
                allFolders[evict].nameCache.clear();
                allFolders[evict].nameCache.shrink_to_fit();
                allFolders[evict].scanned        = false;
                allFolders[evict].nameCacheReady = false;
                allFolders[evict].nameCacheStart  = 0;
                allFolders[evict].trackWindowPage = -1;
                for (int j = i; j < scanLRUCount - 1; j++) scanLRU[j] = scanLRU[j+1];
                scanLRUCount--;
                break;
            }
            evictPass++;
        }

        m4aSrc = new AudioFileSourceM4A();
        aac = new AudioGeneratorAAC();
        if (m4aSrc->open(path.c_str())) {
            m4aSrc->loadSampleSizes();
            aac->begin(m4aSrc, output);
        } else {
            delete m4aSrc; m4aSrc = nullptr;
            delete aac;    aac    = nullptr;
            isPlaying = false;
            drawAll();
            return;
        }
    } else {
        // When browsing recent tracks the path may no longer exist on SD.
        // Normal folder browsing is always in sync so skip the extra lookup there.
        if (isRecentView && !SD.exists(path.c_str())) {
            isPlaying = false;
            showHdrMsg("FILE NOT FOUND");
            drawAll();
            return;
        }
        fileSrc = new AudioFileSourceSD(path.c_str());
        id3Src  = new AudioFileSourceID3(fileSrc);
        mp3     = new AudioGeneratorMP3();
        mp3->begin(id3Src, output);
    }

    isPlaying = true;
    addRecent(path);
    drawAll();
}

// Returns the index of the next track to play, or -1 if none.
// Respects shuffle and repeat modes. Used by pumpAudio and the '/' key.
static int pickNextTrack() {
    std::vector<int> tracks;
    tracks.reserve(items.size());
    for (int i = 0; i < (int)items.size(); i++)
        if (!items[i].isFolder) tracks.push_back(i);
    if (tracks.empty()) return -1;
    if (shuffleOn && tracks.size() > 1) {
        int r;
        do { r = tracks[random(tracks.size())]; } while (r == currentTrack);
        return r;
    }
    int n = currentTrack + 1;
    while (n < (int)items.size() && items[n].isFolder) n++;
    if (n < (int)items.size()) return n;
    if (repeatMode == 2) return tracks[0];
    return -1;
}

void stopAudio() {
    if (mp3)    { if (mp3->isRunning()) mp3->stop(); delete mp3;    mp3    = nullptr; }
    if (aac)    { if (aac->isRunning()) aac->stop(); delete aac;    aac    = nullptr; }
    if (id3Src) { delete id3Src;  id3Src  = nullptr; }
    if (fileSrc){ delete fileSrc; fileSrc = nullptr; }
    if (m4aSrc) { m4aSrc->close(); delete m4aSrc; m4aSrc = nullptr; }
    if (output) output->stop();
    isPlaying = false; isPaused = false;
}

void pauseAudio() {
    if (!isPlaying || isPaused) return;
    pausedElapsedMs += millis() - trackStartMs;
    M5Cardputer.Speaker.stop(0);
    isPlaying = false; isPaused = true;
    drawHeader(); drawStatus();
}

void resumeAudio() {
    if (!isPaused || (!mp3 && !aac)) return;
    output->begin();
    trackStartMs = millis();
    isPlaying = true; isPaused = false;
    drawHeader(); drawStatus();
}

// =============================================================
//  Screen on/off toggle — S key
//  Uses M5GFX setBrightness: 0 = off, 128 = on (mid brightness).
// =============================================================
//  Audio continues uninterrupted when screen is off.
// =============================================================
void toggleScreen() {
    screenOn = !screenOn;
    M5Cardputer.Display.setBrightness(screenOn ? 128 : 0);
    // If turning back on, redraw everything so display is fresh
    if (screenOn) drawAll();
}

// =============================================================
//  Theme switch
// =============================================================
void setTheme(uint8_t idx) {
    if (idx >= 5) return;
    themeIdx = idx;
    T = THEMES[idx];
    settingsDirty = true; settingsDirtyMs = millis();
    drawAll();
    showToast();
}

void showToast() {
    // Centred banner over the screen for 1.5 s
    int tw = SCREEN_W - 40, th = 18, tx = 20, ty = (SCREEN_H - th) / 2;
    M5Cardputer.Display.fillRoundRect(tx, ty, tw, th, 3, T->accent1);
    M5Cardputer.Display.setTextColor(T->bg);
    M5Cardputer.Display.setTextDatum(middle_center);
    M5Cardputer.Display.drawString(T->name, SCREEN_W/2, ty + th/2, 1);
    toastActive = true;
    toastEnd    = millis() + 750;
}

void showHdrMsg(const char* msg) {
    strncpy(hdrMsg, msg, sizeof(hdrMsg) - 1);
    hdrMsg[sizeof(hdrMsg) - 1] = '\0';
    hdrMsgEnd = millis() + 1000;
    drawHeader();
}

void cycleRepeat() {
    repeatMode = (repeatMode + 1) % 3;
    settingsDirty = true; settingsDirtyMs = millis();
    const char* labels[] = { "REPEAT OFF", "REPEAT ONE", "REPEAT ALL" };
    showHdrMsg(labels[repeatMode]);
}

void toggleShuffle() {
    shuffleOn = !shuffleOn;
    settingsDirty = true; settingsDirtyMs = millis();
    showHdrMsg(shuffleOn ? "SHUFFLE ON" : "SHUFFLE OFF");
}

// =============================================================
//  Full repaint — yields to audio between heavy sections
// =============================================================
void drawAll() {
    // No fillScreen() — each sub-function clears its own region atomically,
    // avoiding the full-screen flash that caused glitching.
    drawHeader();
    pumpAudio();
    drawTrackList();
    pumpAudio();
    drawStatus();

    // NEON NOIR: bottom corner brackets are drawn inside statusCanvas (drawStatus)
    // to avoid painting over the status bar sprite.
}

// =============================================================
//  drawHeader  — 0..HEADER_H-1 px
//  Each theme faithfully mirrors its HTML mockup as closely as
//  M5GFX primitives allow.
// =============================================================
void drawHeader() {
    // Draw into off-screen sprite to avoid tearing, then push atomically
    #define D headerCanvas
    D.fillRect(0, 0, SCREEN_W, HEADER_H, T->hdrBg);

    // What to show as the main name in the header:
    String name;
    if (currentTrack >= 0 && currentTrack < (int)items.size())
        name = shortName(items[currentTrack].path, 18);
    else if (viewFolder == "__RECENT__")
        name = "RECENT";
    else if (viewFolder != "/Music")
        name = folderName(viewFolder, 24);
    else
        name = "---";

    // Count string: X/Y tracks only (exclude folders and back item)
    int trackCount = 0, trackPos = 0;
    for (int i = 0; i < (int)items.size(); i++) {
        if (!items[i].isFolder) {
            trackCount++;
            if (i == currentTrack) trackPos = trackCount;
        }
    }
    char countStr[12]; snprintf(countStr, sizeof(countStr), "%d/%d", trackPos, trackCount);
    uint16_t stateCol = isPlaying ? T->accent1 : (isPaused ? T->accent2 : T->textDim);

    // Layout for compact 32px header:
    //   y=7   sys-line / state badge  (font1)
    //   y=22  track name              (font2, 16px tall)
    //   y=30  bottom border

    // ── NEON NOIR ────────────────────────────────────────────
    if (themeIdx == 0) {
        D.fillRect(0, 0, 3, HEADER_H, T->accent1);

        // Top row: blinking state icon | sub-line text | counter
        const char* icon = isPlaying ? ">" : (isPaused ? "||" : "-");
        uint16_t iconCol = (isPlaying && !cursorVisible) ? T->hdrBg : stateCol;
        D.setTextDatum(middle_left);
        D.setTextColor(iconCol);
        D.drawString(icon, 8, 7, 1);

        D.setTextColor(T->textDim);
        String subline;
        if (!isPlaying && !isPaused && currentFolderIdx != 0)
            subline = "/ " + (isRecentView ? String("RECENT") : folderName(viewFolder, 18));
        else
            subline = isPlaying ? "// PLAYING NOW //"
                    : (isPaused ? "// PAUSED //" : "// STOPPED //");
        D.drawString(subline, 20, 7, 1);

        D.setTextColor(T->accent3);
        D.setTextDatum(middle_right);
        D.drawString(countStr, SCREEN_W-4, 7, 1);
        D.setTextColor(T->textDim);
        D.drawString("H", SCREEN_W - 4 - (int)strlen(countStr)*6 - 8, 7, 1);

        // Track name — cyan, font2
        D.setTextColor(T->accent2);
        D.setTextDatum(middle_left);
        D.drawString(name, 8, 22, 2);

        // Bottom border: magenta + faint glow
        D.drawFastHLine(3, HEADER_H-2, SCREEN_W-3, T->accent1);
        D.drawFastHLine(3, HEADER_H-1, SCREEN_W-3, T->textDim);

        // Corner brackets
        D.drawFastHLine(0, 0, 6, T->accent1);
        D.drawFastVLine(0, 0, 6, T->accent1);
        D.drawFastHLine(SCREEN_W-6, 0, 6, T->accent1);
        D.drawFastVLine(SCREEN_W-1, 0, 6, T->accent1);
    }

    // ── GLITCH TERMINAL ──────────────────────────────────────
    else if (themeIdx == 1) {
        // Sys-line top row
        D.setTextDatum(middle_left);
        D.setTextColor(T->textDim);
        D.drawString("BRKN_SIGNAL //", 4, 7, 1);
        D.setTextColor(stateCol);
        String stateTag;
        if (!isPlaying && !isPaused && currentFolderIdx != 0)
            stateTag = isRecentView ? "RECENT" : folderName(viewFolder, 10);
        else
            stateTag = isPlaying ? "PLAYING" : (isPaused ? "PAUSED" : "STOPPED");
        D.drawString(stateTag, 110, 7, 1);

        // Amber counter top-right, with H hint when not playing
        D.setTextColor(T->accent2);
        D.setTextDatum(middle_right);
        char csBracketed[16]; snprintf(csBracketed, sizeof(csBracketed), "[%s]", countStr);
        D.drawString(csBracketed, SCREEN_W-4, 7, 1);
        D.setTextColor(T->textDim);
        int counterW = ((int)strlen(countStr) + 2) * 6 + 8;
        D.drawString("H", SCREEN_W - 4 - counterW, 7, 1);

        // Cursor rect before the name — fixed position, no length dependency
        const int CUR_X = 4, CUR_W = 5, CUR_GAP = 3;
        D.fillRect(CUR_X, 16, CUR_W, 10,
            cursorVisible ? T->accent1 : T->hdrBg);

        // Track name — phosphor green, font2, starts after cursor
        D.setTextColor(T->accent1);
        D.setTextDatum(middle_left);
        D.drawString(name, CUR_X + CUR_W + CUR_GAP, 22, 2);

        // Dashed bottom border
        for (int x = 0; x < SCREEN_W; x += 5)
            D.drawFastHLine(x, HEADER_H-1, 3, T->textDim);
    }

    // ── CORPO CHROME ─────────────────────────────────────────
    else {
        // 3-px gold left bar with brighter top half
        D.fillRect(0, 0, 3, HEADER_H, T->accent1);
        D.drawFastVLine(0, 0, HEADER_H/2, T->accent2);

        // Shallow diagonal accent line near right edge
        for (int row = 0; row < HEADER_H; row++) {
            int lx = SCREEN_W - 36 + row / 4;
            if (lx < SCREEN_W) D.drawPixel(lx, row, T->textDim);
        }

        // State badge top row
        D.setTextDatum(middle_left);
        D.setTextColor(stateCol);
        if (isPlaying) {
            uint16_t dotCol = cursorVisible ? stateCol : T->hdrBg;
            D.setTextColor(dotCol);
            D.drawString("*", 8, 7, 1);
            D.setTextColor(stateCol);
            D.drawString("NOW PLAYING", 18, 7, 1);
        } else if (!isPaused && currentFolderIdx != 0) {
            D.setTextColor(T->textDim);
            D.drawString("/ " + (isRecentView ? String("RECENT") : folderName(viewFolder, 14)), 8, 7, 1);
        } else {
            D.drawString(isPaused ? "* PAUSED" : "* STOPPED", 8, 7, 1);
        }

        // Counter right-aligned, with H hint when not playing
        D.setTextColor(T->accent1);
        D.setTextDatum(middle_right);
        D.drawString(countStr, SCREEN_W-4, 7, 1);
        D.setTextColor(T->textDim);
        D.drawString("H", SCREEN_W - 4 - (int)strlen(countStr)*6 - 8, 7, 1);

        // Track name — chrome, font2
        D.setTextColor(T->textBright);
        D.setTextDatum(middle_left);
        D.drawString(name, 8, 22, 2);

        // Bottom border
        D.drawFastHLine(3, HEADER_H-1, SCREEN_W-3, T->textDim);
    }

    // Header message (repeat/shuffle feedback) — shown below counter, right-aligned.
    // Overrides the scanning indicator slot. Drawn last so it's always on top.
    if (hdrMsgEnd > 0 && millis() < hdrMsgEnd) {
        D.setTextDatum(middle_right);
        D.setTextColor(T->accent2);
        D.drawString(hdrMsg, SCREEN_W-4, 22, 1);
    }

    // ── Per-theme header finishing touches ────────────────────
    if (themeIdx == 3) {
        // Miami Vice: turquoise neon divider at bottom of header
        D.drawFastHLine(0, HEADER_H-2, SCREEN_W, T->accent2);
        D.drawFastHLine(0, HEADER_H-1, SCREEN_W, rgb(0, 80, 70));  // dim echo line
    } else if (themeIdx == 4) {
        // Ash: single clean white bottom border, no glow
        D.drawFastHLine(0, HEADER_H-1, SCREEN_W, T->accent1);
    }

    headerCanvas.pushSprite(0, 0);
    #undef D
}


// =============================================================
//  drawTrackList  — LIST_Y .. STATUS_Y-1
//  Renders folders (with ▶ prefix) and tracks, all 3 themes.
// =============================================================
void drawTrackList() {
    int listH = VISIBLE_TRACKS * LIST_ITEM_H;
    if (items.empty()) {
        M5Cardputer.Display.fillRect(0, HEADER_H, SCREEN_W, STATUS_Y - HEADER_H, T->bg);
        return;
    }

    int top = selectedItem - VISIBLE_TRACKS/2;
    top = max(0, min(top, (int)items.size() - VISIBLE_TRACKS));
    if (top < 0) top = 0;

    int sbW = (themeIdx == 1) ? 7 : 3;

    // Always clear the full scrollbar column (max 7px) before drawing rows.
    // Each row only fills SCREEN_W-sbW wide, so the scrollbar column is never
    // overwritten by row backgrounds. If items fit on one page the scrollbar is
    // not drawn at all, leaving a ghost of whatever was painted by the previous
    // theme or scroll position. One rect call fixes this for all cases.
    M5Cardputer.Display.fillRect(SCREEN_W - 7, LIST_Y, 7, VISIBLE_TRACKS * LIST_ITEM_H, T->bg);

    // Clear only trailing rows not covered by items — avoids the full-area
    // blank flash that caused glitching when scrolling. Each rendered row
    // paints its own background, so no pre-clear is needed for those rows.
    int visibleCount = min(VISIBLE_TRACKS, (int)items.size() - top);
    for (int i = visibleCount; i < VISIBLE_TRACKS; i++) {
        int y = LIST_Y + i * LIST_ITEM_H;
        M5Cardputer.Display.fillRect(0, y, SCREEN_W, LIST_ITEM_H, T->bg);
    }
    // Clear the 1px gap between header sprite boundary and first row
    M5Cardputer.Display.drawFastHLine(0, HEADER_H, SCREEN_W, T->bg);
    // Clear the 1px dead strip at y=STATUS_Y-1 between the last row separator
    // (y=116) and the status sprite (y=118). Never painted by rows, scrollbar,
    // or status — splash remnants survive here otherwise.
    M5Cardputer.Display.drawFastHLine(0, STATUS_Y - 1, SCREEN_W, T->bg);

    // Pre-compute per-theme folder colours
    // Neon Noir:     folders in yellow (accent3)
    // Glitch Term:   folders in amber  (accent2)
    // Corpo Chrome:  folders in gold   (accent2)
    uint16_t folderCol     = T->accent3;   // default (Neon Noir)
    uint16_t folderSelCol  = T->accent2;
    if (themeIdx == 1 || themeIdx == 2) { folderCol = T->accent2; folderSelCol = T->accent1; }

    // Precompute track-number prefix sums so each row is O(1) rather than O(n).
    // tNumPrefix[i] = number of non-folder items in items[0..i-1].
    // Fixed size: PAGE_SIZE + pagination items + subfolders is always < 64.
    int tNumPrefix[64] = {};
    for (int i = 0; i < (int)items.size() && i < 63; i++)
        tNumPrefix[i+1] = tNumPrefix[i] + (items[i].isFolder ? 0 : 1);

    for (int i = 0; i < VISIBLE_TRACKS; i++) {
        int idx = top + i;
        if (idx >= (int)items.size()) break;

        int  y        = LIST_Y + i * LIST_ITEM_H;
        bool sel      = (idx == selectedItem);
        bool play     = (!items[idx].isFolder && idx == currentTrack);
        bool isFolder = items[idx].isFolder;

        // ── Pagination items: BACK / MORE ─────────────────────
        // Drawn as a simple centred label in dim accent colour, no folder decorations.
        bool isPagination = (items[idx].path == "__PREV__" || items[idx].path == "__MORE__");
        if (isPagination) {
            M5Cardputer.Display.fillRect(0, y, SCREEN_W, LIST_ITEM_H,
                sel ? T->selRow : T->bg);
            uint16_t pc = sel ? T->accent1 : T->textDim;
            M5Cardputer.Display.setTextColor(pc);
            M5Cardputer.Display.setTextDatum(middle_center);
            M5Cardputer.Display.drawString(items[idx].label, SCREEN_W/2, y + LIST_ITEM_H/2, 1);
            M5Cardputer.Display.setTextDatum(middle_left);
            continue;
        }

        // ── Row background ────────────────────────────────────
        if (themeIdx == 0) {
            // Neon Noir: 2-tone gradient sim for selected; folder rows get a
            // faint yellow tint left half
            if (sel) {
                M5Cardputer.Display.fillRect(0,          y, SCREEN_W/2,     LIST_ITEM_H, T->selRow);
                M5Cardputer.Display.fillRect(SCREEN_W/2, y, SCREEN_W/2, LIST_ITEM_H, T->bg);
            } else if (isFolder) {
                M5Cardputer.Display.fillRect(0, y, SCREEN_W, LIST_ITEM_H, rgb(14,12,4));
            } else {
                M5Cardputer.Display.fillRect(0, y, SCREEN_W, LIST_ITEM_H, T->bg);
            }
            M5Cardputer.Display.drawFastHLine(0, y+LIST_ITEM_H-1, SCREEN_W, rgb(9,21,32));

        } else if (themeIdx == 1) {
            // Glitch Terminal: folder rows get a very faint amber tint
            uint16_t rowBg = sel      ? T->selRow
                           : isFolder ? rgb(10,8,0)
                                      : T->bg;
            M5Cardputer.Display.fillRect(0, y, SCREEN_W, LIST_ITEM_H, rowBg);

        } else {
            // Corpo Chrome: selected gold tint; folder rows subtle gold tint
            if (sel) {
                M5Cardputer.Display.fillRect(0,  y, 80,           LIST_ITEM_H, rgb(20,16,10));
                M5Cardputer.Display.fillRect(80, y, SCREEN_W-80, LIST_ITEM_H, T->bg);
            } else if (isFolder) {
                M5Cardputer.Display.fillRect(0, y, SCREEN_W, LIST_ITEM_H, rgb(14,11,4));
            } else {
                M5Cardputer.Display.fillRect(0, y, SCREEN_W, LIST_ITEM_H, T->bg);
            }
            M5Cardputer.Display.drawFastHLine(0, y+LIST_ITEM_H-1, SCREEN_W, T->textDim);
        }

        // ── Left indicator bar ────────────────────────────────
        if (themeIdx == 0) {
            if (play)
                M5Cardputer.Display.fillRect(3, y, 3, LIST_ITEM_H, T->accent1);
            else if (sel && isFolder)
                M5Cardputer.Display.fillRect(3, y, 2, LIST_ITEM_H, folderSelCol);
            else if (sel)
                M5Cardputer.Display.fillRect(3, y, 2, LIST_ITEM_H, T->accent2);
        } else if (themeIdx == 2) {
            if (play)
                M5Cardputer.Display.fillRect(3, y+3, 3, LIST_ITEM_H-6, T->accent1);
            else if (sel && isFolder)
                M5Cardputer.Display.fillRect(3, y+3, 3, LIST_ITEM_H-6, folderSelCol);
            if (sel) {
                M5Cardputer.Display.drawFastHLine(SCREEN_W-sbW-7, y+3, 5, T->accent2);
                M5Cardputer.Display.drawFastVLine(SCREEN_W-sbW-3, y+3, 5, T->accent2);
            }
        } else if (themeIdx == 3) {
            // Miami Vice: playing = full pink bar; selected = turquoise bar
            if (play)
                M5Cardputer.Display.fillRect(0, y, 3, LIST_ITEM_H, T->accent1);
            else if (sel)
                M5Cardputer.Display.fillRect(0, y, 3, LIST_ITEM_H, T->accent2);
        } else if (themeIdx == 4) {
            // Ash: selected = single white pixel-wide left edge line (text-editor cursor)
            if (sel)
                M5Cardputer.Display.drawFastVLine(0, y, LIST_ITEM_H, T->accent1);
            if (play)
                M5Cardputer.Display.drawFastVLine(1, y, LIST_ITEM_H, T->accent2);
        }
        // Terminal indicator drawn in the prefix column below

        // ── Prefix column (index or terminal glyph) ──────────
        int midY = y + LIST_ITEM_H/2;

        if (isFolder) {
            // All themes: show folder icon
            if (themeIdx == 1) {
                // Terminal: "#" marks directories (distinct from track ">" / "|")
                M5Cardputer.Display.setTextDatum(middle_left);
                M5Cardputer.Display.setTextColor(sel ? folderSelCol : folderCol);
                M5Cardputer.Display.drawString("#", 2, midY, 1);
            }
        } else {
            // Track: number or terminal glyph
            if (themeIdx == 1) {
                M5Cardputer.Display.setTextDatum(middle_left);
                if (play) {
                    M5Cardputer.Display.setTextColor(T->accent1);
                    M5Cardputer.Display.drawString(">", 2, midY, 1);
                } else if (sel) {
                    M5Cardputer.Display.setTextColor(T->accent2);
                    M5Cardputer.Display.drawString("|", 2, midY, 1);
                }
                M5Cardputer.Display.setTextColor(T->textDim);
                int tNum = tNumPrefix[idx + 1];
                char numBuf[4]; snprintf(numBuf, sizeof(numBuf), "%02d", tNum);
                M5Cardputer.Display.drawString(numBuf, 12, midY, 1);
            } else {
                int tNum = tNumPrefix[idx + 1];
                char numBuf[4]; snprintf(numBuf, sizeof(numBuf), "%02d", tNum);
                // Ash: numbers always dim, independent of selection/play state
                uint16_t nc = (themeIdx == 4) ? T->textDim
                            : play ? T->accent1 : (sel ? T->accent2 : T->textDim);
                M5Cardputer.Display.setTextColor(nc);
                M5Cardputer.Display.setTextDatum(middle_left);
                M5Cardputer.Display.drawString(numBuf, 8, midY, 1);
            }
        }

        // ── Label ─────────────────────────────────────────────
        M5Cardputer.Display.setTextDatum(middle_left);
        int nameX = (themeIdx == 1) ? 30 : 26;

        if (isFolder) {
            // Folder: "/ NAME" — slash prefix is terminal path style,
            // distinct from track ">" (playing) and "|" (selected)
            uint16_t fc = sel ? folderSelCol : folderCol;
            M5Cardputer.Display.setTextColor(fc);
            M5Cardputer.Display.drawString("/", nameX, midY, 1);
            String lbl = items[idx].label;
            int maxFolderChars = (themeIdx == 1) ? 32 : 33;
            if ((int)lbl.length() > maxFolderChars) lbl = lbl.substring(0, maxFolderChars - 1) + ">";
            M5Cardputer.Display.setTextColor(fc);
            M5Cardputer.Display.drawString(lbl, nameX+10, midY, 1);
        } else {
            // Track name
            uint16_t nameCol;
            if (themeIdx == 1)
                nameCol = play ? T->accent1 : (sel ? T->accent2 : T->textMid);
            else
                nameCol = play ? T->accent1 : (sel ? T->textBright : T->textMid);
            M5Cardputer.Display.setTextColor(nameCol);
            String lbl = items[idx].label;
            // Corpo Chrome: reserve ~30px on the right for the duration display
            int maxTrackChars = (themeIdx == 1) ? 33 : (themeIdx == 2) ? 30 : 35;
            if ((int)lbl.length() > maxTrackChars) lbl = lbl.substring(0, maxTrackChars - 1) + ">";
            M5Cardputer.Display.drawString(lbl, nameX, midY, 1);
        }

        // ── Per-theme row extras ───────────────────────────────

        // Neon Noir: scanline glow — 1px accent lines above+below the playing row
        if (themeIdx == 0 && play) {
            uint16_t glowCol = rgb(80, 0, 40);   // very dim magenta
            M5Cardputer.Display.drawFastHLine(0, y,                  SCREEN_W, glowCol);
            M5Cardputer.Display.drawFastHLine(0, y + LIST_ITEM_H - 1, SCREEN_W, glowCol);
        }

        // Glitch Terminal: blinking block cursor at right edge of selected row
        if (themeIdx == 1 && sel && !isFolder) {
            uint16_t cur = cursorVisible ? T->accent1 : T->bg;
            M5Cardputer.Display.fillRect(SCREEN_W - sbW - 8, y + 3, 5, LIST_ITEM_H - 6, cur);
        }

        // Corpo Chrome: right-aligned track duration in textDim
        if (themeIdx == 2 && !isFolder && items[idx].durationMs > 0) {
            char dur[8]; { unsigned long s=items[idx].durationMs/1000; snprintf(dur,sizeof(dur),"%02lu:%02lu",s/60,s%60); }
            M5Cardputer.Display.setTextDatum(middle_right);
            M5Cardputer.Display.setTextColor(play ? T->accent1 : T->textDim);
            M5Cardputer.Display.drawString(dur, SCREEN_W - sbW - 2, midY, 1);
            M5Cardputer.Display.setTextDatum(middle_left);
        }
    }

    // ── Scrollbar ──────────────────────────────────────────────
    if ((int)items.size() > VISIBLE_TRACKS) {
        int sbH = listH;
        int thH = max(5, sbH * VISIBLE_TRACKS / (int)items.size());
        int thY = LIST_Y + (sbH - thH) * top
                  / max(1, (int)items.size() - VISIBLE_TRACKS);
        int sbX = SCREEN_W - sbW;
        if (themeIdx == 1) {
            M5Cardputer.Display.drawFastVLine(sbX, LIST_Y, sbH, T->textDim);
            M5Cardputer.Display.fillRect(sbX+1, thY, sbW-1, thH, T->textDim);
            M5Cardputer.Display.drawRect( sbX+1, thY, sbW-1, thH, T->textMid);
        } else {
            M5Cardputer.Display.fillRect(sbX, LIST_Y, sbW, sbH, T->barBg);
            M5Cardputer.Display.fillRect(sbX, thY,    sbW, thH,
                themeIdx == 2 ? T->accent2 : T->accent1);
        }
    }
}

// =============================================================
//  drawStatus  — STATUS_Y..SCREEN_H-1, rendered via sprite
// =============================================================
void drawStatus() {
    unsigned long elapsed = pausedElapsedMs + (isPlaying ? millis() - trackStartMs : 0UL);
    float prog = (trackDurationMs > 0)
                 ? min(1.0f, (float)elapsed / (float)trackDurationMs) : 0.0f;
    int volPct = (volume * 100) / 255;

    statusCanvas.fillSprite(T->hdrBg);

    // ── NEON NOIR ─────────────────────────────────────────────
    // Mockup: cyan elapsed | dim total | magenta→cyan gradient bar | yellow VOL val
    //         corner brackets bottom-left (cyan) bottom-right (cyan)
    if (themeIdx == 0) {
        const int BAR_X = 72, BAR_W = 120, BAR_H = 6;  // ends at 192, 4px before BAT label
        const int BAR_Y = (STATUS_H - BAR_H) / 2;

        // Top border — simulate glow from header
        statusCanvas.drawFastHLine(0, 0, SCREEN_W, T->textDim);

        // Times
        statusCanvas.setTextDatum(middle_left);
        statusCanvas.setTextColor(T->accent2);           // cyan elapsed
        statusCanvas.drawString(formatTime(elapsed), 4, STATUS_H/2, 1);
        statusCanvas.setTextColor(T->textDim);
        statusCanvas.drawString(formatTime(trackDurationMs), 40, STATUS_H/2, 1);

        // Progress bar trough
        statusCanvas.fillRect(BAR_X, BAR_Y, BAR_W, BAR_H, T->barBg);
        statusCanvas.drawRect(BAR_X, BAR_Y, BAR_W, BAR_H, rgb(14,48,64));

        // Fill: magenta body + 2px cyan tip (simulate gradient)
        int fill = (int)(BAR_W * prog);
        if (fill > 3) {
            statusCanvas.fillRect(BAR_X,        BAR_Y, fill-2, BAR_H, T->accent1);
            statusCanvas.fillRect(BAR_X+fill-2, BAR_Y, 2,      BAR_H, T->accent2);
        } else if (fill > 0) {
            statusCanvas.fillRect(BAR_X, BAR_Y, fill, BAR_H, T->accent1);
        }

        // Right column stacked: VOL top (y=5), BAT% bottom (y=13) — zero overlap
        char vbuf[8]; snprintf(vbuf, sizeof(vbuf), "VOL %d", volPct);
        statusCanvas.setTextColor(T->accent3);
        statusCanvas.setTextDatum(middle_right);
        statusCanvas.drawString(vbuf, SCREEN_W-2, 5, 1);
        if (batteryLevel >= 0) {
            char bbuf[8]; snprintf(bbuf, sizeof(bbuf), "BAT %d%%", batteryLevel);
            statusCanvas.setTextColor(T->textMid);  // brighter than textDim, clear against bg
            statusCanvas.drawString(bbuf, SCREEN_W-2, 13, 1);
        }

        // Bottom corner brackets (cyan) — left side only; right removed to avoid covering BAT%
        statusCanvas.drawFastHLine(0,        STATUS_H-1, 5, T->accent2);
        statusCanvas.drawFastVLine(0,        STATUS_H-5, 5, T->accent2);
    }

    // ── GLITCH TERMINAL ───────────────────────────────────────
    // Mockup: "POS>" label | large green elapsed | / dim total | block bar | VOL:amber
    //         second row: "SPC:PAUSE  +/-:VOL  ENTER:PLAY  STATE:■ ACTIVE" in dim/mid
    else if (themeIdx == 1) {
        const int BAR_X = 100, BAR_W = 92, BAR_H = 6;  // ends at 192, 4px before VOL: label
        const int BAR_Y = (STATUS_H - BAR_H) / 2;

        // Top border dashes
        for (int x = 0; x < SCREEN_W; x += 5)
            statusCanvas.drawFastHLine(x, 0, 3, T->textDim);

        // POS> label
        statusCanvas.setTextDatum(middle_left);
        statusCanvas.setTextColor(T->textDim);
        statusCanvas.drawString("POS>", 2, STATUS_H/2, 1);

        // Elapsed — green
        statusCanvas.setTextColor(T->accent1);
        statusCanvas.drawString(formatTime(elapsed), 26, STATUS_H/2, 1);

        // Separator + total — fixed at x=56, total at x=62, bar starts at 100
        statusCanvas.setTextColor(T->textDim);
        statusCanvas.drawString("/", 56, STATUS_H/2, 1);
        statusCanvas.setTextColor(T->textMid);
        statusCanvas.drawString(formatTime(trackDurationMs), 62, STATUS_H/2, 1);

        // Segmented block bar (each segment 4px wide, 1px gap)
        int segs    = BAR_W / 5;
        int litSegs = (int)(segs * prog);
        for (int s = 0; s < segs; s++) {
            int bx = BAR_X + s * 5;
            statusCanvas.fillRect(bx, BAR_Y, 4, BAR_H,
                s < litSegs ? T->accent1 : T->barBg);
        }

        // Right column stacked: VOL:XX top (y=5), B:XX% bottom (y=13) — zero overlap
        statusCanvas.setTextDatum(middle_right);
        statusCanvas.setTextColor(T->textDim);
        char vtag[10]; snprintf(vtag, sizeof(vtag), "VOL:%d", volPct);
        statusCanvas.drawString(vtag, SCREEN_W-2, 5, 1);
        if (batteryLevel >= 0) {
            char bbuf[8]; snprintf(bbuf, sizeof(bbuf), "B:%d%%", batteryLevel);
            statusCanvas.setTextColor(T->accent2);
            statusCanvas.drawString(bbuf, SCREEN_W-2, 13, 1);
        }
    }

    // ── CORPO CHROME ──────────────────────────────────────────
    // Mockup: time stacked (large current / small total below) | diagonal sep |
    //         skewed progress bar with tick marks | vol number large gold / "VOL" label
    else if (themeIdx == 2) {
        // Top border
        statusCanvas.drawFastHLine(0, 0, SCREEN_W, T->textDim);

        // Time group: elapsed / total stacked — push down slightly from top border
        statusCanvas.setTextDatum(middle_left);
        statusCanvas.setTextColor(T->textBright);
        statusCanvas.drawString(formatTime(elapsed), 4, STATUS_H/2 - 2, 1);
        statusCanvas.setTextColor(T->textDim);
        statusCanvas.drawString("/" + formatTime(trackDurationMs), 4, STATUS_H/2 + 6, 1);

        // Diagonal separator (skewed line)
        for (int row = 2; row < STATUS_H-2; row++) {
            int dx = (row - 2) / 3;
            statusCanvas.drawPixel(64 + dx, row, T->textDim);
        }

        // Progress bar — BAR_W=122 ends at 194, 4px before right column (198)
        const int BAR_X = 72, BAR_W = 122, BAR_H = 7;
        const int BAR_Y = (STATUS_H - BAR_H) / 2;
        statusCanvas.fillRect(BAR_X, BAR_Y, BAR_W, BAR_H, T->barBg);
        int fill = (int)(BAR_W * prog);
        if (fill > 0) statusCanvas.fillRect(BAR_X, BAR_Y, fill, BAR_H, T->accent1);
        // Tick marks every 10%
        for (int t = 1; t < 10; t++) {
            int tx = BAR_X + BAR_W * t / 10;
            statusCanvas.drawFastVLine(tx, BAR_Y+1, BAR_H-2,
                t * 10 <= (int)(prog*100) ? T->bg : T->textMid);
        }

        // Right column: "VOL" label + vol number y=5; "BAT" label + bat% y=13
        // Values in accent2 (gold-bright), labels in textMid for readable contrast
        char vbuf[6]; snprintf(vbuf, sizeof(vbuf), "%d", volPct);
        statusCanvas.setTextDatum(middle_right);
        statusCanvas.setTextColor(T->accent2);
        statusCanvas.drawString(vbuf, SCREEN_W-2, 5, 1);
        statusCanvas.setTextColor(T->textMid);
        statusCanvas.drawString("VOL", SCREEN_W-2-(int)strlen(vbuf)*6-4, 5, 1);
        if (batteryLevel >= 0) {
            char bbuf[6]; snprintf(bbuf, sizeof(bbuf), "%d%%", batteryLevel);
            statusCanvas.setTextColor(T->textBright);
            statusCanvas.drawString(bbuf, SCREEN_W-2, 13, 1);
            statusCanvas.setTextColor(T->textMid);
            statusCanvas.drawString("BAT", SCREEN_W-2-(int)strlen(bbuf)*6-4, 13, 1);
        } else {
            statusCanvas.setTextColor(T->textMid);
            statusCanvas.drawString("VOL", SCREEN_W-2, 13, 1);
        }
    }

    // ── MIAMI VICE ────────────────────────────────────────────
    // Split-colour bar: hot pink top half, turquoise bottom half.
    // Thin turquoise top border (neon divider line).
    else if (themeIdx == 3) {
        const int BAR_X = 72, BAR_W = 120, BAR_H = 6;  // ends at 192, 4px before BAT label
        const int BAR_Y = (STATUS_H - BAR_H) / 2;

        // Neon divider line at top
        statusCanvas.drawFastHLine(0, 0, SCREEN_W, T->accent2);

        // Times
        statusCanvas.setTextDatum(middle_left);
        statusCanvas.setTextColor(T->accent2);           // turquoise elapsed
        statusCanvas.drawString(formatTime(elapsed), 4, STATUS_H/2, 1);
        statusCanvas.setTextColor(T->textDim);
        statusCanvas.drawString(formatTime(trackDurationMs), 40, STATUS_H/2, 1);

        // Trough
        statusCanvas.fillRect(BAR_X, BAR_Y, BAR_W, BAR_H, T->barBg);

        // Fill: pink top half, turquoise bottom half
        int fill = (int)(BAR_W * prog);
        if (fill > 0) {
            int half = BAR_H / 2;
            statusCanvas.fillRect(BAR_X, BAR_Y,        fill, half,        T->accent1);  // pink
            statusCanvas.fillRect(BAR_X, BAR_Y + half, fill, BAR_H - half, T->accent2); // turquoise
        }

        // Right column stacked: VOL top (y=5), BAT% bottom (y=13) — zero overlap
        char vbuf[8]; snprintf(vbuf, sizeof(vbuf), "VOL %d", volPct);
        statusCanvas.setTextColor(T->accent3);
        statusCanvas.setTextDatum(middle_right);
        statusCanvas.drawString(vbuf, SCREEN_W-2, 5, 1);
        if (batteryLevel >= 0) {
            char bbuf[8]; snprintf(bbuf, sizeof(bbuf), "BAT %d%%", batteryLevel);
            statusCanvas.setTextColor(T->accent2);
            statusCanvas.drawString(bbuf, SCREEN_W-2, 13, 1);
        }
    }

    // ── ASH ───────────────────────────────────────────────────
    // Clean white bar on dark trough, no colour. Elapsed / total in grey.
    // Volume shown as small filled blocks (no text label).
    else if (themeIdx == 4) {
        // elapsed = 5 chars * 6px = 30px, starts at x=4, ends at x=34
        // gap of 4px, total starts at x=38, ends at x=68
        // bar starts at x=72 to clear both times comfortably
        // bar ends at 172, 4px before vol blocks which start at 176 (with bat: 240-2-28-34=176)
        const int BAR_X = 72, BAR_W = 100, BAR_H = 3;  // ends at 172, clears vol blocks + bat%
        const int BAR_Y = (STATUS_H - BAR_H) / 2;

        // Thin top border
        statusCanvas.drawFastHLine(0, 0, SCREEN_W, T->textDim);

        // Times — bright elapsed, dim total, no overlap
        statusCanvas.setTextDatum(middle_left);
        statusCanvas.setTextColor(T->textBright);
        statusCanvas.drawString(formatTime(elapsed), 4, STATUS_H/2, 1);
        statusCanvas.setTextColor(T->textDim);
        statusCanvas.drawString(formatTime(trackDurationMs), 38, STATUS_H/2, 1);

        // Minimal bar — white fill on dark trough, no border
        statusCanvas.fillRect(BAR_X, BAR_Y, BAR_W, BAR_H, T->barBg);
        int fill = (int)(BAR_W * prog);
        if (fill > 0)
            statusCanvas.fillRect(BAR_X, BAR_Y, fill, BAR_H, T->accent1);

        // Volume: 6 filled pixel blocks (4px each, 2px gap) | BAT% in dim text
        int maxBlocks = 6;
        int litBlocks = (int)(volPct * maxBlocks / 100.0f + 0.5f);
        int blockW = 4, blockGap = 2;
        // Battery "99%" = 18px, "|" = 6px, gap = 4px → 28px reserved on the right
        // Blocks: 6*(4+2)-2 = 34px, starting at SCREEN_W-2-28-34 = 176
        int batW   = (batteryLevel >= 0) ? 28 : 0;  // "| XX%" space (only if reading available)
        int totalW = maxBlocks * (blockW + blockGap) - blockGap;
        int bx0    = SCREEN_W - 2 - batW - totalW;
        for (int b = 0; b < maxBlocks; b++) {
            int bx = bx0 + b * (blockW + blockGap);
            uint16_t bc = (b < litBlocks) ? T->accent2 : T->textDim;
            statusCanvas.fillRect(bx, BAR_Y - 1, blockW, BAR_H + 2, bc);
        }
        if (batteryLevel >= 0) {
            char bbuf[6]; snprintf(bbuf, sizeof(bbuf), "%d%%", batteryLevel);
            statusCanvas.setTextColor(T->textDim);
            statusCanvas.setTextDatum(middle_left);
            int sepX = bx0 + totalW + 4;
            statusCanvas.drawString("|", sepX, STATUS_H/2, 1);
            statusCanvas.drawString(bbuf, sepX + 8, STATUS_H/2, 1);
        }
    }

    statusCanvas.pushSprite(0, STATUS_Y);
}

// =============================================================
//  Help overlay  — full-screen sprite, toggled by H
// =============================================================
void toggleHelp() {
    // Help draws directly to display (no sprite) — safe during M4A playback.
    // drawHelp() takes ~15ms, well within the 69ms audio buffer depth.
    helpVisible = !helpVisible;
    if (helpVisible) {
        drawHelp();
    } else {
        drawAll();
    }
}

void drawHelp() {
    auto& D = M5Cardputer.Display;

    D.fillRect(0, 0, SCREEN_W, SCREEN_H, T->bg);
    D.drawRect(4, 4, SCREEN_W-8,  SCREEN_H-8,  T->accent1);
    D.drawRect(5, 5, SCREEN_W-10, SCREEN_H-10, T->textDim);

    struct Row { const char* key; const char* desc; };
    static const Row rows[] = {
        { "ENTER",   "Open folder / Play track" },
        { "DEL",     "Back to parent folder"    },
        { "SPACE",   "Pause / Resume"           },
        { "; / .",   "Cursor up / down"         },
        { ", / /",   "Prev / Next"             },
        { "+ / -",   "Volume up / down"         },
        { "R",       "Cycle repeat mode"        },
        { "S",       "Toggle shuffle"           },
        { "1-5",     "Switch theme"             },
        { "O",       "Screen on / off"          },
        { "H",       "Close this screen"        },
    };
    const int rows_n = sizeof(rows)/sizeof(rows[0]);
    const int startY = 8;
    const int rowH   = (SCREEN_H - startY*2) / rows_n;

    for (int i = 0; i < rows_n; i++) {
        int y = startY + i*rowH + rowH/2;
        if (i % 2 == 0)
            D.fillRect(6, startY + i*rowH, SCREEN_W-12, rowH, T->selRow);
        D.setTextDatum(middle_left);
        D.setTextColor(T->accent2);
        D.drawString(rows[i].key,  12, y, 1);
        D.setTextColor(T->textMid);
        D.drawString(rows[i].desc, 68, y, 1);
    }
}

// =============================================================
//  Recent tracks — circular front-push list, persisted to SD
// =============================================================
void addRecent(const String& path) {
    // Remove existing entry for this path (dedupe)
    int existing = -1;
    for (int i = 0; i < recentCount; i++)
        if (recentPaths[i] == path) { existing = i; break; }
    if (existing >= 0) {
        for (int i = existing; i > 0; i--)
            recentPaths[i] = recentPaths[i-1];
    } else {
        int slots = min(recentCount, RECENT_MAX - 1);
        for (int i = slots; i > 0; i--)
            recentPaths[i] = recentPaths[i-1];
        if (recentCount < RECENT_MAX) recentCount++;
    }
    recentPaths[0] = path;
    saveRecentToSD();
}

void loadRecentView() {
    isRecentView = true;
    viewFolder   = "__RECENT__";
    currentTrack = -1;
    selectedItem = 0;
    items.clear();
    for (int i = 0; i < recentCount; i++) {
        BrowserItem bi;
        bi.isFolder   = false;
        bi.path       = recentPaths[i];
        bi.fileSize   = 0;
        bi.durationMs = 0;

        // Try to find matching TrackEntry in already-scanned folders
        bool found = false;
        for (const FolderEntry& fe : allFolders) {
            if (!fe.scanned) continue;
            for (const TrackEntry& te : fe.tracks) {
                if (te.path == recentPaths[i]) {
                    bi.fileSize   = te.fileSize;
                    bi.durationMs = te.durationMs;
                    found = true;
                    break;
                }
            }
            if (found) break;
        }

        // Not in scanned folders — read duration directly from file header.
        // Also verifies the file still exists; skip silently if it doesn't.
        if (!found) {
            File f = SD.open(recentPaths[i].c_str());
            if (!f) continue;   // file removed from SD — skip this recent entry
            bi.fileSize = f.size();
            f.close();
            String lo = recentPaths[i]; lo.toLowerCase();
            if (lo.endsWith(".m4a"))
                bi.durationMs = readM4ADuration(recentPaths[i].c_str());
            else
                bi.durationMs = readMP3Duration(recentPaths[i].c_str(), bi.fileSize);
        }

        // Build label from path
        int sl  = recentPaths[i].lastIndexOf('/');
        String n = (sl >= 0) ? recentPaths[i].substring(sl+1) : recentPaths[i];
        int dot = n.lastIndexOf('.');
        if (dot > 0) n = n.substring(0, dot);
        n.replace('_', ' ');
        if ((int)n.length() > 19) n = n.substring(0, 18) + ">";
        bi.label = n;
        items.push_back(bi);
    }
}

void saveRecentToSD() {
    File f = SD.open("/recent.txt", FILE_WRITE);
    if (!f) return;
    for (int i = 0; i < recentCount; i++) {
        f.println(recentPaths[i]);
    }
    f.close();
}

void loadRecentFromSD() {
    recentCount = 0;
    File f = SD.open("/recent.txt", FILE_READ);
    if (!f) return;
    while (f.available() && recentCount < RECENT_MAX) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() > 0) {
            recentPaths[recentCount++] = line;
        }
    }
    f.close();
}

// =============================================================
//  Settings persistence — /settings.cfg
//  Format: one key=value per line.  Written only when not playing.
// =============================================================
void saveSettings() {
    File f = SD.open("/Music/settings.cfg", FILE_WRITE);
    if (!f) return;
    f.printf("theme=%d\n",  themeIdx);
    f.printf("volume=%d\n", volume);
    f.printf("repeat=%d\n", repeatMode);
    f.printf("shuffle=%d\n", shuffleOn ? 1 : 0);
    f.close();
}

void loadSettings() {
    File f = SD.open("/Music/settings.cfg", FILE_READ);
    if (!f) return;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        int eq = line.indexOf('=');
        if (eq < 0) continue;
        String key = line.substring(0, eq);
        int    val = line.substring(eq + 1).toInt();
        if (key == "theme"  && val >= 0 && val < 5) { themeIdx = val; T = THEMES[val]; }
        if (key == "volume" && val >= 0 && val <= 255) { volume = (uint8_t)val; M5Cardputer.Speaker.setVolume(volume); }
        if (key == "repeat" && val >= 0 && val <= 2)   repeatMode = (uint8_t)val;
        if (key == "shuffle") shuffleOn = (val != 0);
    }
    f.close();
}
String folderName(const String& p, int maxCh) {
    int sl = p.lastIndexOf('/');
    String n = (sl >= 0) ? p.substring(sl+1) : p;
    n.replace('_', ' ');
    if ((int)n.length() > maxCh) n = n.substring(0, maxCh-1) + ">";
    return n;
}

String shortName(const String &p, int maxCh) {
    int sl = p.lastIndexOf('/');
    String n = (sl >= 0) ? p.substring(sl+1) : p;
    int dot = n.lastIndexOf('.');
    if (dot > 0) n = n.substring(0, dot);
    n.replace('_', ' ');
    if ((int)n.length() > maxCh) n = n.substring(0, maxCh-1) + ">";
    return n;
}

String formatTime(unsigned long ms) {
    unsigned long s = ms / 1000;
    char b[8]; snprintf(b, sizeof(b), "%02lu:%02lu", s/60, s%60);
    return String(b);
}
