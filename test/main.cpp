// MusyX song synthesis demo
//
// Plays a MusyX song from the four data files (proj, pool, samp, sdir) and
// writes the mixed output to a stereo WAV file.
//
// Data files exported from GameCube / Wii titles are in big-endian format.
// This program automatically byte-swaps all multi-byte fields to native
// (little-endian) order before passing the data to the MusyX library.
//
// NOTE: salBuildCommandList is currently a TODO stub in the PC target, so the
//       mix buffers (dspStudio[0].main[]) are never filled with synthesised
//       audio and the output WAV will therefore be silent.  All other MusyX
//       subsystems (sequencer, synthesiser, ADSR envelopes, etc.) still run
//       correctly.  Once salBuildCommandList is implemented for PC, this
//       program will produce real audio without any further changes.
//
// Usage:
//   test --proj <file> --pool <file> --samp <file> --sdir <file>
//        [--arr <file>] [--group <id>] [--songid <id>]
//        [--out <file>] [--duration <secs>]

#include <musyx/dspvoice.h>
#include <musyx/musyx.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// ---------------------------------------------------------------------------
// External PC-backend symbols (hw_pc.c / hardware.c)
// ---------------------------------------------------------------------------
extern "C" {
// Drives one full audio-processing frame (calls snd_handle_irq internally).
void salCallback();
}

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// Sample rate set by salInitAi for the PC target.
static constexpr uint32_t kSampleRate = 32000;

// 5 time-slots per hardware IRQ × 32 samples per time-slot.
static constexpr uint32_t kSamplesPerFrame = 160;

// Stereo output.
static constexpr uint16_t kChannels = 2;
static constexpr uint16_t kBitsPerSample = 16;

// ---------------------------------------------------------------------------
// Minimal WAV writer
// ---------------------------------------------------------------------------

class WavWriter {
public:
  explicit WavWriter(const std::string& path) : m_out(path, std::ios::binary) {
    if (!m_out) {
      throw std::runtime_error("Cannot create WAV file: " + path);
    }
    // Reserve space for the 44-byte PCM WAV header; written properly in
    // Finalize().
    m_out.write(reinterpret_cast<const char*>(m_header.data()), m_header.size());
  }

  // Write one stereo frame.
  // left/right must each have exactly kSamplesPerFrame s32 elements.
  // The s32 values are treated as signed fixed-point: the upper 16 bits are
  // used as the s16 PCM sample (right-shift by 16, then clamp).
  void WriteFrame(const s32* left, const s32* right) {
    for (uint32_t i = 0; i < kSamplesPerFrame; ++i) {
      auto l = static_cast<int16_t>(std::clamp(left[i] >> 16, -32768, 32767));
      auto r = static_cast<int16_t>(std::clamp(right[i] >> 16, -32768, 32767));
      m_out.write(reinterpret_cast<const char*>(&l), sizeof(l));
      m_out.write(reinterpret_cast<const char*>(&r), sizeof(r));
    }
    ++m_totalFrames;
  }

  // Seek back to the beginning and write the complete RIFF/WAV header.
  void Finalize() {
    const uint32_t totalSamples = m_totalFrames * kSamplesPerFrame;
    const uint32_t dataBytes    = totalSamples * kChannels * (kBitsPerSample / 8);
    const uint32_t chunkSize    = 36 + dataBytes;
    const uint32_t byteRate     = kSampleRate * kChannels * (kBitsPerSample / 8);
    const uint16_t blockAlign   = static_cast<uint16_t>(kChannels * (kBitsPerSample / 8));
    const uint16_t audioFmt     = 1; // PCM

    auto w16 = [&](uint32_t off, uint16_t v) { std::memcpy(&m_header[off], &v, 2); };
    auto w32 = [&](uint32_t off, uint32_t v) { std::memcpy(&m_header[off], &v, 4); };

    std::memcpy(&m_header[0], "RIFF", 4);
    w32(4, chunkSize);
    std::memcpy(&m_header[8], "WAVE", 4);
    std::memcpy(&m_header[12], "fmt ", 4);
    w32(16, 16); // fmt chunk size
    w16(20, audioFmt);
    w16(22, kChannels);
    w32(24, kSampleRate);
    w32(28, byteRate);
    w16(32, blockAlign);
    w16(34, kBitsPerSample);
    std::memcpy(&m_header[36], "data", 4);
    w32(40, dataBytes);

    m_out.seekp(0);
    m_out.write(reinterpret_cast<const char*>(m_header.data()), m_header.size());
  }

private:
  std::ofstream           m_out;
  std::array<uint8_t, 44> m_header{};
  uint64_t                m_totalFrames = 0;
};

// ---------------------------------------------------------------------------
// File loading helpers
// ---------------------------------------------------------------------------

// Load a file into a std::vector<uint8_t>.  The caller owns the buffer via
// the vector's lifetime – suitable for proj, pool, samp, arr data since the
// library does not take ownership of those pointers.
static std::vector<uint8_t> LoadFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    throw std::runtime_error("Cannot open: " + path);
  }
  const auto sz = static_cast<std::size_t>(f.tellg());
  f.seekg(0);
  std::vector<uint8_t> buf(sz);
  if (!f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(sz))) {
    throw std::runtime_error("Read error: " + path);
  }
  return buf;
}

// ---------------------------------------------------------------------------
// Big-endian byte-swap helpers
// ---------------------------------------------------------------------------
//
// MusyX data files exported from GameCube/Wii titles are stored in big-endian
// byte order.  The functions below convert every multi-byte field in-place to
// native (little-endian) byte order so the MusyX library can use them
// directly on x86/x86-64 hosts.
//
// Byte-order note: 0xFFFF / 0xFFFFFFFF sentinel values are palindromes –
// their byte representation is identical in both byte orders – so they can be
// detected both before and after swapping without special treatment.

static uint16_t readBE16(const uint8_t* p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

static uint32_t readBE32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

// Swap 2 bytes at pointer in-place.
static void bswap16p(uint8_t* p) {
  uint8_t t = p[0];
  p[0]      = p[1];
  p[1]      = t;
}

// Swap 4 bytes at pointer in-place.
static void bswap32p(uint8_t* p) {
  uint8_t t0 = p[0];
  uint8_t t1 = p[1];
  p[0]       = p[3];
  p[1]       = p[2];
  p[2]       = t1;
  p[3]       = t0;
}

// Swap a u16 array terminated by 0xFFFF (project ID lists).
static void byteswapU16List(uint8_t* p, const uint8_t* end) {
  while (p + 2 <= end) {
    uint16_t v = readBE16(p);
    bswap16p(p);
    if (v == 0xFFFF) {
      break;
    }
    p += 2;
  }
}

// Swap PAGE entries.  PAGE = { u16 macro, u8 prio, u8 maxVoices, u8 index, u8 reserved }
// Terminated by index == 0xFF.
static void byteswapPageArray(uint8_t* p, const uint8_t* end) {
  while (p + 6 <= end) {
    bswap16p(p); // macro (u16)
    // prio, maxVoices, index, reserved are all u8 – no swap needed
    if (p[4] == 0xFF) { // index field (after swap, still a single byte)
      break;
    }
    p += 6;
  }
}

// Swap MIDISETUP entries.  MIDISETUP = { u16 songId, u16 reserved, MIDI_CHANNEL_SETUP[16] }
// MIDI_CHANNEL_SETUP is 5 × u8.  Total size = 4 + 80 = 84 = 0x54 bytes.
// Terminated by songId == 0xFFFF.
static void byteswapMidiSetupArray(uint8_t* p, const uint8_t* end) {
  while (p + 0x54 <= end) {
    uint16_t sid = readBE16(p);
    bswap16p(p);     // songId
    bswap16p(p + 2); // reserved
    // MIDI_CHANNEL_SETUP channel[16] are all u8 – no swap needed
    if (sid == 0xFFFF) {
      break;
    }
    p += 0x54;
  }
}

// ---------------------------------------------------------------------------
// Project file byte-swap  (GROUP_DATA linked list + all pointed-to data)
// ---------------------------------------------------------------------------
// GROUP_DATA layout (0x28 bytes):
//   u32 nextOff, u16 id, u16 type,
//   u32 macroOff, sampleOff, curveOff, keymapOff, layerOff
//   union { { u32 tableOff }; { u32 normpageOff, drumpageOff, midiSetupOff } }

static void byteswapProjectData(std::vector<uint8_t>& buf) {
  uint8_t* data = buf.data();
  uint8_t* end  = data + buf.size();
  uint8_t* g    = data;

  while (g + 0x28 <= end) {
    const uint32_t nextOff = readBE32(g + 0x00); // read before swap
    const uint16_t gtype   = readBE16(g + 0x06); // read type before swap

    bswap32p(g + 0x00); // nextOff
    bswap16p(g + 0x04); // id
    bswap16p(g + 0x06); // type
    bswap32p(g + 0x08); // macroOff
    bswap32p(g + 0x0C); // sampleOff
    bswap32p(g + 0x10); // curveOff
    bswap32p(g + 0x14); // keymapOff
    bswap32p(g + 0x18); // layerOff
    bswap32p(g + 0x1C); // normpageOff / tableOff
    if (gtype == 0) {   // Song group – swap the remaining two u32 fields
      bswap32p(g + 0x20); // drumpageOff
      bswap32p(g + 0x24); // midiSetupOff
    }

    // After the swap, read the now-native offsets using memcpy
    auto rd32 = [](const uint8_t* p) -> uint32_t {
      uint32_t v;
      std::memcpy(&v, p, 4);
      return v;
    };

    const uint32_t off_macro  = rd32(g + 0x08);
    const uint32_t off_sample = rd32(g + 0x0C);
    const uint32_t off_curve  = rd32(g + 0x10);
    const uint32_t off_keymap = rd32(g + 0x14);
    const uint32_t off_layer  = rd32(g + 0x18);

    // Swap u16 ID lists (terminated by 0xFFFF)
    auto swapList = [&](uint32_t off) {
      if (off && data + off + 2 <= end) {
        byteswapU16List(data + off, end);
      }
    };
    swapList(off_sample);
    swapList(off_macro);
    swapList(off_curve);
    swapList(off_keymap);
    swapList(off_layer);

    if (gtype == 0) { // Song group
      const uint32_t off_norm = rd32(g + 0x1C);
      const uint32_t off_drum = rd32(g + 0x20);
      const uint32_t off_midi = rd32(g + 0x24);
      if (off_norm && data + off_norm <= end) {
        byteswapPageArray(data + off_norm, end);
      }
      if (off_drum && data + off_drum <= end) {
        byteswapPageArray(data + off_drum, end);
      }
      if (off_midi && data + off_midi <= end) {
        byteswapMidiSetupArray(data + off_midi, end);
      }
    }

    if (nextOff == 0xFFFFFFFF) {
      break;
    }
    g += nextOff;
  }
}

// Return the ID of the first group in an already-swapped project buffer,
// or 0xFFFF if the buffer is empty/invalid.
static uint16_t getFirstGroupId(const std::vector<uint8_t>& buf) {
  if (buf.size() < 0x06) {
    return 0xFFFF;
  }
  uint16_t id;
  std::memcpy(&id, buf.data() + 0x04, 2); // id field at offset 4 (already native)
  return id;
}

// ---------------------------------------------------------------------------
// Pool file byte-swap  (POOL_DATA header + MEM_DATA linked lists)
// ---------------------------------------------------------------------------
// POOL_DATA = { u32 macroOff, curveOff, keymapOff, layerOff } at offset 0.
// MEM_DATA  = { u32 nextOff, u16 id, u16 reserved, data union }.
//   Macros:  MSTEP pairs  (u32[2] each) – swap all u32 in the data area
//   Curves:  u8 tab       – no swap needed
//   Keymaps: KEYMAP[128]  = (u16 id, s8, u8, s16, u8[2]) × 128
//   Layers:  u32 num + LAYER[num]  = (u16 id, u8×4, s16, u8×4) × num

static void byteswapMemDataList(uint8_t* start, const uint8_t* secEnd, uint8_t dataType) {
  uint8_t* m = start;
  while (m + 8 <= secEnd) {
    const uint32_t nextOff = readBE32(m + 0x00);
    const uint8_t* dataEnd =
        (nextOff != 0xFFFFFFFF) ? m + nextOff : secEnd; // data runs to next entry or section end

    bswap32p(m + 0x00); // nextOff
    bswap16p(m + 0x04); // id
    bswap16p(m + 0x06); // reserved

    uint8_t* d = m + 8; // start of data area

    if (dataType == 0) {
      // Macros: MSTEP = u32 para[2].  Swap every u32.
      while (d + 4 <= dataEnd) {
        bswap32p(d);
        d += 4;
      }
    } else if (dataType == 2) {
      // Keymaps: KEYMAP = u16 id + s8 + u8 + s16 + u8[2], 8 bytes each, 128 entries.
      for (int i = 0; i < 128 && d + 8 <= dataEnd; ++i, d += 8) {
        bswap16p(d + 0); // id
        bswap16p(d + 4); // prioOffset (s16)
      }
    } else if (dataType == 3) {
      // Layers: u32 num, then LAYER[num] where LAYER = u16 id + u8×4 + s16 + u8×4 (12 bytes)
      if (d + 4 <= dataEnd) {
        const uint32_t num = readBE32(d);
        bswap32p(d);
        d += 4;
        for (uint32_t i = 0; i < num && d + 12 <= dataEnd; ++i, d += 12) {
          bswap16p(d + 0); // id
          bswap16p(d + 6); // prioOffset (s16)
        }
      }
    }
    // dataType == 4 (curves): u8 tab – no swap

    if (nextOff == 0xFFFFFFFF) {
      break;
    }
    m += nextOff;
  }
}

static void byteswapPoolData(std::vector<uint8_t>& buf) {
  uint8_t* data = buf.data();
  const uint8_t* end = data + buf.size();
  if (buf.size() < 16) {
    return;
  }

  // Read section offsets before swapping the POOL_DATA header
  const uint32_t macroOff  = readBE32(data + 0x00);
  const uint32_t curveOff  = readBE32(data + 0x04);
  const uint32_t keymapOff = readBE32(data + 0x08);
  const uint32_t layerOff  = readBE32(data + 0x0C);

  bswap32p(data + 0x00); // macroOff
  bswap32p(data + 0x04); // curveOff
  bswap32p(data + 0x08); // keymapOff
  bswap32p(data + 0x0C); // layerOff

  // Helper to get the start of the next section (lowest offset > secStart)
  auto nextSecStart = [&](uint32_t secStart) -> const uint8_t* {
    uint32_t best = static_cast<uint32_t>(buf.size());
    for (uint32_t off : {macroOff, curveOff, keymapOff, layerOff}) {
      if (off > secStart && off < best) {
        best = off;
      }
    }
    return data + best;
  };

  if (macroOff && macroOff < buf.size()) {
    byteswapMemDataList(data + macroOff, nextSecStart(macroOff), 0);
  }
  if (curveOff && curveOff < buf.size()) {
    byteswapMemDataList(data + curveOff, nextSecStart(curveOff), 4);
  }
  if (keymapOff && keymapOff < buf.size()) {
    byteswapMemDataList(data + keymapOff, nextSecStart(keymapOff), 2);
  }
  if (layerOff && layerOff < buf.size()) {
    byteswapMemDataList(data + layerOff, end, 3);
  }
}

// ---------------------------------------------------------------------------
// SDIR file byte-swap  (must be called before sndConvert32BitSDIRTo64BitSDIR)
// ---------------------------------------------------------------------------
// SDIR_DATA_INTER = { u16 id, u16 ref_cnt, u32 offset, u32 addr,
//                     SAMPLE_HEADER{u32×4}, u32 extraData }  (0x20 bytes each)
// Terminated by id == 0xFFFF.

static void byteswapSdirData(void* rawBuf, std::size_t size) {
  auto* p   = static_cast<uint8_t*>(rawBuf);
  auto* end = p + size;
  while (p + 0x20 <= end) {
    const uint16_t id = readBE16(p);
    bswap16p(p + 0x00); // id
    bswap16p(p + 0x02); // ref_cnt
    bswap32p(p + 0x04); // offset
    bswap32p(p + 0x08); // addr (u32 – will be widened to void* by sndConvert)
    bswap32p(p + 0x0C); // SAMPLE_HEADER.info
    bswap32p(p + 0x10); // SAMPLE_HEADER.length
    bswap32p(p + 0x14); // SAMPLE_HEADER.loopOffset
    bswap32p(p + 0x18); // SAMPLE_HEADER.loopLength
    bswap32p(p + 0x1C); // extraData
    if (id == 0xFFFF) {
      break;
    }
    p += 0x20;
  }
}

// ---------------------------------------------------------------------------
// ARR / song file byte-swap  (.son files)
// ---------------------------------------------------------------------------
// ARR struct (0x58 bytes):
//   u32 tTab, pTab, tmTab, mTrack, info, loopPoint[16], tsTab
//
// All offsets in ARR fields are relative to the ARR struct pointer itself.
//
// .son files exported from MusyX typically have 0×N leading zeroes before
// the first valid ARR (N is a multiple of 0x58, the ARR struct size).
// findArrStart() scans forward to the first ARR with a plausible info field.
//
// Some fields (e.g. tmTab, tsTab, and late pTab entries in StarFox data)
// reference data past the physical end of the .son file.  On GameCube,
// the adjacent memory was zero-filled.  The caller is expected to extend
// arrBuf with ARR_PADDING zero-bytes before calling this function; the
// paddingSize parameter tells this function how many extra bytes were added
// so it can place NOTE_DATA terminators in the padding for any out-of-bounds
// SEQ_PATTERN entries.

static constexpr std::size_t ARR_PADDING = 512; // bytes appended to arrBuf

static std::size_t findArrStart(const uint8_t* data, std::size_t size) {
  const std::size_t ARR_SIZE = 0x58;
  for (std::size_t off = 0; off + ARR_SIZE <= size; off += ARR_SIZE) {
    const uint32_t tTab = readBE32(data + off + 0x00);
    const uint32_t info = readBE32(data + off + 0x10);
    // A valid ARR has tTab >= ARR_SIZE (track table follows the header) and
    // a non-zero info field (contains BPM and flags).
    if (tTab >= ARR_SIZE && info != 0) {
      return off;
    }
  }
  return 0; // fallback: use the whole buffer as-is
}

static void byteswapArrData(uint8_t* arr, std::size_t size, std::size_t paddingSize) {
  static constexpr std::size_t ARR_SIZE      = 0x58;
  static constexpr std::size_t TENTRY_SIZE   = 0x0C;
  static constexpr std::size_t MTRACK_SIZE   = 0x08;
  static constexpr std::size_t SEQ_PAT_HDR   = 0x0C; // three leading u32s in SEQ_PATTERN

  if (size < ARR_SIZE) {
    return;
  }
  uint8_t* end     = arr + size;                  // original data limit
  uint8_t* extEnd  = arr + size + paddingSize;    // extended limit (includes padding)

  // Read header fields before swapping
  const uint32_t tTab   = readBE32(arr + 0x00);
  const uint32_t pTab   = readBE32(arr + 0x04);
  const uint32_t mTrack = readBE32(arr + 0x0C);

  // Swap ARR header
  bswap32p(arr + 0x00); // tTab
  bswap32p(arr + 0x04); // pTab
  bswap32p(arr + 0x08); // tmTab
  bswap32p(arr + 0x0C); // mTrack
  bswap32p(arr + 0x10); // info
  for (int i = 0; i < 16; ++i) {
    bswap32p(arr + 0x14 + i * 4); // loopPoint[16]
  }
  bswap32p(arr + 0x54); // tsTab

  // Swap tracktab (64 × u32 at arr+tTab) and all TENTRY arrays.
  // Collect the maximum TENTRY pattern index to know the pTab count.
  //
  // IMPORTANT: Multiple tracks can share overlapping TENTRY data.  A naïve
  // per-track swap would byte-swap shared entries multiple times.  We therefore
  // do this in two passes:
  //   Pass A – read-only scan to determine maxPat (no mutations).
  //   Pass B – swap tracktab u32s and TENTRY fields, using a high-water mark
  //            so each TENTRY byte is mutated at most once.
  // In both passes the while loop is guarded with `te < arr + pTab` so we never
  // walk past the TENTRY region into the pTab array.
  uint32_t maxPat = 0;

  if (tTab + 64 * 4 <= size) {
    uint8_t* ttab = arr + tTab;
    uint8_t* pTabLimit = arr + pTab; // first byte of pTab array – hard upper bound

    // Pass A: read-only – collect maxPat from big-endian values still in buffer.
    for (int ti = 0; ti < 64; ++ti) {
      const uint32_t trackOff = readBE32(ttab + ti * 4); // still BE here
      if (trackOff == 0 || arr + trackOff + TENTRY_SIZE > end) continue;

      uint8_t* te = arr + trackOff;
      while (te + TENTRY_SIZE <= end && te < pTabLimit) {
        const uint16_t pat = readBE16(te + 0x08); // still BE
        if (pat == 0xFFFF) break;
        if (pat < 0xFFFE) maxPat = std::max(maxPat, static_cast<uint32_t>(pat));
        te += TENTRY_SIZE;
      }
    }

    // Pass B: swap tracktab entries and TENTRY arrays.
    // Use hwm (high-water mark) to prevent double-swapping overlapping ranges.
    // The pTabLimit guard prevents the hwm from advancing into the pTab array.
    uint8_t* hwm = nullptr; // highest address we have already fully processed
    for (int ti = 0; ti < 64; ++ti) {
      const uint32_t trackOff = readBE32(ttab + ti * 4);
      bswap32p(ttab + ti * 4);

      if (trackOff == 0 || arr + trackOff + TENTRY_SIZE > end) continue;

      // Only process TENTRY entries that start at or after the hwm.
      uint8_t* te = arr + trackOff;
      if (hwm && te < hwm) {
        te = hwm; // skip already-swapped entries
      }

      while (te + TENTRY_SIZE <= end && te < pTabLimit) {
        const uint16_t pat = readBE16(te + 0x08); // still BE at te >= hwm

        bswap32p(te + 0x00); // time (u32)
        bswap16p(te + 0x08); // pattern (u16)

        te += TENTRY_SIZE;
        if (hwm == nullptr || te > hwm) hwm = te;

        if (pat == 0xFFFF) break;
        if (pat == 0xFFFE) {
          // Loop-back: the two s8 bytes encode a u16 loop-back index BE.
          bswap16p(te - TENTRY_SIZE + 0x0A);
        }
      }
    }
  }

  // Swap pTab (u32 array with at least maxPat+1 entries).
  // For any entry that points beyond the original data boundary, place an
  // empty-but-valid SEQ_PATTERN (header zeros + NOTE_DATA terminator) in the
  // padding area so the sequencer runtime doesn't read garbage or loop forever.
  if (pTab + (maxPat + 1) * 4 <= size) {
    uint8_t* ptab = arr + pTab;
    for (uint32_t pi = 0; pi <= maxPat; ++pi) {
      const uint32_t patOff = readBE32(ptab + pi * 4);
      bswap32p(ptab + pi * 4);

      if (patOff == 0) {
        continue;
      }

      uint8_t* sp = arr + patOff;

      if (sp + SEQ_PAT_HDR > end) {
        // Out-of-bounds SEQ_PATTERN: synthesise a valid empty pattern in the
        // padding area so the runtime hits a NOTE_DATA terminator immediately.
        if (sp + SEQ_PAT_HDR + 4 <= extEnd) {
          std::memset(sp, 0, SEQ_PAT_HDR);  // pitchBend=0, modulation=0
          // NOTE_DATA terminator: time=0, key=0xFF, vel=0xFF
          sp[SEQ_PAT_HDR + 0] = 0x00;
          sp[SEQ_PAT_HDR + 1] = 0x00;
          sp[SEQ_PAT_HDR + 2] = 0xFF;
          sp[SEQ_PAT_HDR + 3] = 0xFF;
        }
        continue;
      }

      // Swap SEQ_PATTERN header (headerLen, pitchBend offset, modulation offset)
      bswap32p(sp + 0x00); // headerLen
      bswap32p(sp + 0x04); // pitchBend stream offset (ARR-relative)
      bswap32p(sp + 0x08); // modulation stream offset (ARR-relative)

      // Swap NOTE_DATA array starting immediately at offset 0x0C.
      // NOTE_DATA = { u16 time, u8 key, u8 velocity, u16 length } (6 bytes) OR
      //             { u16 time, u8 key|0x80, u8 velocity }         (4 bytes special)
      // Terminated by key==0xFF && velocity==0xFF.
      uint8_t* nd = sp + SEQ_PAT_HDR;
      while (nd + 4 <= end) {
        bswap16p(nd + 0); // time (u16) – always swap
        const uint8_t key = nd[2];
        const uint8_t vel = nd[3];

        if (key == 0xFF && vel == 0xFF) {
          break; // terminator
        }
        if ((key & 0x80) || (key == 0 && vel == 0)) {
          nd += 4; // 4-byte event
        } else {
          if (nd + 6 <= end) {
            bswap16p(nd + 4); // length (u16)
          }
          nd += 6; // 6-byte regular note
        }
      }
      // Pitch-bend and modulation streams are byte-granular – no swap needed.
    }
  }

  // Swap MTRACK_DATA array (u32 time, u32 bpm, terminated by time==0xFFFFFFFF)
  if (mTrack != 0 && arr + mTrack + MTRACK_SIZE <= end) {
    uint8_t* mt = arr + mTrack;
    while (mt + MTRACK_SIZE <= end) {
      const uint32_t t = readBE32(mt + 0x00);
      bswap32p(mt + 0x00); // time
      bswap32p(mt + 0x04); // bpm
      if (t == 0xFFFFFFFF) {
        break;
      }
      mt += MTRACK_SIZE;
    }
  }
  // tmTab (u8[64]) and tsTab (u8[64]) are byte arrays – no swap needed.
  // If their ARR-relative offsets exceed 'size', the zero-filled padding
  // provides all-zero MIDI channel / section assignments, which is valid.
}


struct Options {
  std::string             proj;
  std::string             pool;
  std::string             samp;
  std::string             sdir;
  std::optional<std::string> arr;       // Sequencer arrangement file (.song) – optional
  std::string             out         = "output.wav";
  std::optional<uint16_t> groupId;      // auto-detected from proj if not specified
  uint16_t                songId      = 0;
  uint32_t                duration    = 30; // seconds to render
};

static void PrintUsage(const char* argv0) {
  std::cerr << "Usage: " << argv0 << "\n"
            << "  Required:\n"
            << "    --proj <file>      Project file (.proj)\n"
            << "    --pool <file>      Pool data file (.pool)\n"
            << "    --samp <file>      PCM sample data file (.samp)\n"
            << "    --sdir <file>      Sample directory file (.sdir)\n"
            << "  Optional:\n"
            << "    --arr  <file>      Sequencer arrangement file (.song)\n"
            << "    --group  <id>      Group ID to play (default: first group in .proj)\n"
            << "    --songid <id>      Song ID within the group (default: 0)\n"
            << "    --out  <file>      Output WAV path (default: output.wav)\n"
            << "    --duration <secs>  Render duration in seconds (default: 30)\n";
}

static std::optional<Options> ParseArgs(int argc, char* argv[]) {
  Options opts;
  for (int i = 1; i < argc; ++i) {
    const std::string_view key = argv[i];
    auto requireNext           = [&]() -> std::string_view {
      if (++i >= argc) {
        throw std::runtime_error(std::string("Missing value for option: ") + std::string(key));
      }
      return argv[i];
    };

    if (key == "--proj") {
      opts.proj = requireNext();
    } else if (key == "--pool") {
      opts.pool = requireNext();
    } else if (key == "--samp") {
      opts.samp = requireNext();
    } else if (key == "--sdir") {
      opts.sdir = requireNext();
    } else if (key == "--arr") {
      opts.arr = std::string(requireNext());
    } else if (key == "--out") {
      opts.out = requireNext();
    } else if (key == "--group") {
      opts.groupId = static_cast<uint16_t>(std::stoul(std::string(requireNext())));    } else if (key == "--songid") {
      opts.songId = static_cast<uint16_t>(std::stoul(std::string(requireNext())));
    } else if (key == "--duration") {
      opts.duration = static_cast<uint32_t>(std::stoul(std::string(requireNext())));
    } else {
      std::cerr << "Unknown option: " << key << "\n";
      return std::nullopt;
    }
  }

  if (opts.proj.empty() || opts.pool.empty() || opts.samp.empty() || opts.sdir.empty()) {
    return std::nullopt;
  }
  return opts;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
  std::optional<Options> opts;
  try {
    opts = ParseArgs(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << "Argument error: " << e.what() << "\n";
    PrintUsage(argv[0]);
    return 1;
  }
  if (!opts) {
    PrintUsage(argv[0]);
    return 1;
  }
  // -------------------------------------------------------------------------
  std::vector<uint8_t> projBuf, poolBuf, sampBuf, sdirVec;
  std::vector<uint8_t> arrFileData; // raw file contents, only used temporarily

  try {
    projBuf = LoadFile(opts->proj);
    poolBuf = LoadFile(opts->pool);
    sampBuf = LoadFile(opts->samp);
    sdirVec = LoadFile(opts->sdir); // loaded into a vector for easy byte-swapping
    if (opts->arr) {
      arrFileData = LoadFile(*opts->arr);
    }
  } catch (const std::exception& e) {
    std::cerr << "Error loading files: " << e.what() << "\n";
    return 1;
  }

  // -------------------------------------------------------------------------
  // Byte-swap all data from big-endian (GameCube/Wii) to native byte order.
  // -------------------------------------------------------------------------
  byteswapProjectData(projBuf);
  byteswapPoolData(poolBuf);
  byteswapSdirData(sdirVec.data(), sdirVec.size());

  // Auto-detect group ID from the (already-swapped) project file if not given
  const uint16_t groupId =
      opts->groupId.has_value() ? *opts->groupId : getFirstGroupId(projBuf);

  // Find and byte-swap the ARR data (.son files start with leading zeroes
  // before the actual ARR struct; findArrStart() locates it).
  // The buffer is extended with ARR_PADDING zero-bytes so that ARR fields
  // which reference data slightly past the physical end of the file (a common
  // artefact in GameCube-exported .son files) read as zeros rather than
  // causing a segfault.  byteswapArrData() also patches a NOTE_DATA terminator
  // into the padding for any SEQ_PATTERN entries that land there.
  //
  // NOTE: We deliberately use malloc() here rather than std::vector::resize()
  // because AddressSanitizer marks the extra bytes added by resize() as
  // "container-overflow" (poisoned), which causes a SEGV when the MusyX
  // runtime accesses the padding.  A plain malloc'd allocation has no such
  // annotation and allows the runtime to read the zeroed padding safely.
  std::size_t arrOffset = 0;
  std::unique_ptr<uint8_t, decltype(&std::free)> arrExtBuf{nullptr, std::free};
  if (!arrFileData.empty()) {
    const std::size_t fileSize     = arrFileData.size();
    const std::size_t extSize      = fileSize + ARR_PADDING;
    uint8_t* raw = static_cast<uint8_t*>(std::malloc(extSize));
    if (!raw) {
      std::cerr << "Out of memory allocating ARR buffer\n";
      return 1;
    }
    std::memcpy(raw, arrFileData.data(), fileSize);
    std::memset(raw + fileSize, 0, ARR_PADDING);
    arrFileData.clear(); // release original vector memory
    arrExtBuf.reset(raw);

    arrOffset = findArrStart(raw, fileSize); // scan original data only
    byteswapArrData(raw + arrOffset, fileSize - arrOffset, ARR_PADDING);
  }

  // sndConvert32BitSDIRTo64BitSDIR() requires a malloc'd buffer (it calls
  // free() on it).  Copy the byte-swapped SDIR data into a malloc'd block.
  void* sdirRaw = std::malloc(sdirVec.size());
  if (!sdirRaw) {
    std::cerr << "Out of memory allocating SDIR buffer\n";
    return 1;
  }
  std::memcpy(sdirRaw, sdirVec.data(), sdirVec.size());
  sdirVec.clear();

  // Convert 32-bit SDIR address fields to native 64-bit pointers.
  // sndConvert32BitSDIRTo64BitSDIR() takes ownership of sdirRaw (frees it)
  // and returns a freshly malloc'd, converted SDIR structure.
  void* sdirNative = sndConvert32BitSDIRTo64BitSDIR(sdirRaw);
  sdirRaw = nullptr; // no longer valid after the call above
  if (!sdirNative) {
    std::cerr << "sndConvert32BitSDIRTo64BitSDIR failed\n";
    return 1;
  }

  // -------------------------------------------------------------------------
  // Initialise MusyX
  // -------------------------------------------------------------------------
  SND_HOOKS hooks{malloc, free};
  sndSetHooks(&hooks);

  // sndInit() returns 0 on success.
  // Parameters: voices, maxMusic, maxSFX, studios, flags, aramSize.
  // aramSize is unused on the PC target (aramInit is a no-op).
  if (sndInit(64, 64, 64, 8, 0, 0) != 0) {
    std::cerr << "sndInit failed\n";
    std::free(sdirNative);
    return 1;
  }

  sndOutputMode(SND_OUTPUTMODE_STEREO);
  sndVolume(127, 0, SND_ALL_VOLGROUPS);

  // -------------------------------------------------------------------------
  // Push the group data (registers samples, macros, curves, keymaps, layers)
  // -------------------------------------------------------------------------
  if (!sndPushGroup(projBuf.data(), groupId, sampBuf.data(), sdirNative, poolBuf.data())) {
    std::cerr << "sndPushGroup failed – group ID " << groupId << " not found in project\n";
    sndQuit();
    std::free(sdirNative);
    return 1;
  }

  // -------------------------------------------------------------------------
  // Start song playback via the sequencer
  // -------------------------------------------------------------------------
  // arrPtr is optional sequencer data (the arrangement / .song file).
  // Pass nullptr if not provided; the library may embed the arrangement inside
  // the project file.
  // arrPtr points to the actual ARR struct within the buffer (skipping the
  // leading zeroes present in .son files exported from MusyX tools).
  void* arrPtr = arrExtBuf ? static_cast<void*>(arrExtBuf.get() + arrOffset) : nullptr;
  const SND_SEQID seqId = sndSeqPlay(groupId, opts->songId, arrPtr, nullptr);
  if (seqId == SND_ID_ERROR) {
    std::cerr << "sndSeqPlay failed – verify that group " << groupId
              << " is a Song group and that song ID " << opts->songId << " exists\n";
    sndPopGroup();
    sndQuit();
    std::free(sdirNative);
    return 1;
  }

  std::cout << "Playing song " << opts->songId << " from group " << groupId
            << "  (seqId=" << seqId << ")\n";

  // -------------------------------------------------------------------------
  // Render loop
  // -------------------------------------------------------------------------
  // salCallback() drives one hardware-IRQ equivalent: it calls snd_handle_irq
  // internally, which in turn calls the sequencer (seqHandle), synthesiser
  // (synthHandle), 3-D audio (s3dHandle), and streaming subsystems.
  //
  // The completed PCM mix for studio 0 sits in:
  //   dspStudio[0].main[frameIdx ^ 1][0   .. 159]  – left  channel (s32)
  //   dspStudio[0].main[frameIdx ^ 1][160 .. 319]  – right channel (s32)
  //   dspStudio[0].main[frameIdx ^ 1][320 .. 479]  – surround (not used here)
  //
  // frameIdx mirrors the internal salFrame variable which snd_handle_irq
  // toggles on every call.  The "just completed" buffer is therefore at
  // main[frameIdx ^ 1] after the toggle.
  //
  // IMPORTANT: salBuildCommandList (which fills these buffers) is currently
  // a TODO stub for the PC target, so all samples will be zero until it is
  // implemented.  The WAV file will be structurally correct but silent.

  const uint32_t totalFrames = (opts->duration * kSampleRate) / kSamplesPerFrame;
  std::cout << "Rendering " << opts->duration << "s  →  " << totalFrames << " frames  →  "
            << opts->out << "\n";

  // Local mirror of the internal salFrame counter (starts at 0 after sndInit).
  uint8_t frameIdx = 0;

  try {
    WavWriter wav(opts->out);

    for (uint32_t f = 0; f < totalFrames; ++f) {
      // Advance MusyX by one audio frame.
      salCallback();

      // Mirror the salFrame toggle that happens inside snd_handle_irq.
      frameIdx ^= 1;

      // Read the just-completed mix buffers.
      const s32* frameBuf  = dspStudio[0].main[frameIdx ^ 1];
      const s32* leftChan  = frameBuf;
      const s32* rightChan = frameBuf + kSamplesPerFrame;
      wav.WriteFrame(leftChan, rightChan);

      // Stop early once the sequencer has finished (song ended naturally).
      if (!sndSeqGetValid(seqId)) {
        std::cout << "Sequence ended at frame " << f << "\n";
        break;
      }
    }

    wav.Finalize();
  } catch (const std::exception& e) {
    std::cerr << "Render error: " << e.what() << "\n";
    sndSeqStop(seqId);
    sndPopGroup();
    sndQuit();
    std::free(sdirNative);
    return 1;
  }

  std::cout << "Done. Written to " << opts->out << "\n";

  // -------------------------------------------------------------------------
  // Cleanup
  // -------------------------------------------------------------------------
  sndSeqStop(seqId);
  sndPopGroup();
  sndQuit();
  std::free(sdirNative);
  return 0;
}
