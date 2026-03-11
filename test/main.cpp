// MusyX song synthesis demo
//
// Plays a MusyX song from the four data files (proj, pool, samp, sdir) and
// writes the mixed output to a stereo WAV file.
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

// Load a file into a malloc'd buffer and return the raw pointer.
// Used for the SDIR file only, because sndConvert32BitSDIRTo64BitSDIR()
// takes ownership of (i.e. calls free() on) its input pointer and returns a
// freshly malloc'd converted buffer.
static void* LoadFileMalloc(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    throw std::runtime_error("Cannot open: " + path);
  }
  const auto sz = static_cast<std::size_t>(f.tellg());
  f.seekg(0);
  void* buf = std::malloc(sz);
  if (!buf) {
    throw std::bad_alloc();
  }
  if (!f.read(static_cast<char*>(buf), static_cast<std::streamsize>(sz))) {
    std::free(buf);
    throw std::runtime_error("Read error: " + path);
  }
  return buf;
}

// ---------------------------------------------------------------------------
// Command-line parsing
// ---------------------------------------------------------------------------

struct Options {
  std::string             proj;
  std::string             pool;
  std::string             samp;
  std::string             sdir;
  std::optional<std::string> arr;       // Sequencer arrangement file (.song) – optional
  std::string             out      = "output.wav";
  uint16_t                groupId  = 1;
  uint16_t                songId   = 0;
  uint32_t                duration = 30; // seconds to render
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
            << "    --group  <id>      Group ID to play (default: 1)\n"
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
      opts.groupId = static_cast<uint16_t>(std::stoul(std::string(requireNext())));
    } else if (key == "--songid") {
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
  std::vector<uint8_t> projBuf, poolBuf, sampBuf, arrBuf;
  void* sdirRaw = nullptr; // malloc'd; consumed by sndConvert32BitSDIRTo64BitSDIR

  try {
    projBuf = LoadFile(opts->proj);
    poolBuf = LoadFile(opts->pool);
    sampBuf = LoadFile(opts->samp);
    sdirRaw = LoadFileMalloc(opts->sdir);
    if (opts->arr) {
      arrBuf = LoadFile(*opts->arr);
    }
  } catch (const std::exception& e) {
    std::cerr << "Error loading files: " << e.what() << "\n";
    std::free(sdirRaw);
    return 1;
  }

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
  if (!sndPushGroup(projBuf.data(), opts->groupId, sampBuf.data(), sdirNative, poolBuf.data())) {
    std::cerr << "sndPushGroup failed – check group ID " << opts->groupId << "\n";
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
  void* arrPtr = arrBuf.empty() ? nullptr : static_cast<void*>(arrBuf.data());
  const SND_SEQID seqId = sndSeqPlay(opts->groupId, opts->songId, arrPtr, nullptr);
  if (seqId == SND_ID_ERROR) {
    std::cerr << "sndSeqPlay failed – verify that group " << opts->groupId
              << " is a Song group and that song ID " << opts->songId << " exists\n";
    sndPopGroup();
    sndQuit();
    std::free(sdirNative);
    return 1;
  }

  std::cout << "Playing song " << opts->songId << " from group " << opts->groupId
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
