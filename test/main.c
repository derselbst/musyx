#include <musyx/musyx.h>

#include <musyx/seq.h>

#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <musyx/sal.h>

namespace {
constexpr SND_GROUPID kGroupId = 1;

bool loadFile(const char* path, std::vector<std::uint8_t>& data, std::string& error) {
  FILE* file = std::fopen(path, "rb");
  if (file == nullptr) {
    error = std::string("Failed to open file: ") + path;
    return false;
  }

  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  if (size < 0) {
    std::fclose(file);
    error = std::string("Failed to get file size: ") + path;
    return false;
  }
  if (size == 0) {
    std::fclose(file);
    error = std::string("File is empty: ") + path;
    return false;
  }

  std::rewind(file);
  data.resize(static_cast<std::size_t>(size));
  const std::size_t readSize = std::fread(data.data(), 1, data.size(), file);
  std::fclose(file);
  if (readSize != data.size()) {
    error = std::string("Failed to read file fully: ") + path;
    data.clear();
    return false;
  }
  return true;
}

void printUsage(const char* exe) {
  std::fprintf(stderr,
               "Usage: %s <proj> <pool> <samp> <sdir> [song] [song-id] [wav-out]\n"
               "If [song] is omitted, <proj>.song in the same directory is used.\n",
               exe);
}

std::string defaultSongPath(const char* projPath) {
  std::string path(projPath);
  const std::size_t slash = path.find_last_of("/\\");
  std::size_t dot = path.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
    dot = path.size();
  }
  path.replace(dot, std::string::npos, ".song");
  return path;
}

std::string defaultWavPath(const std::string& songPath) {
  std::string path(songPath);
  const std::size_t slash = path.find_last_of("/\\");
  std::size_t dot = path.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
    dot = path.size();
  }
  path.replace(dot, std::string::npos, ".wav");
  return path;
}

bool parseSongId(const char* text, SND_SONGID& songId) {
  errno = 0;
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(text, &end, 0);
  if (errno != 0 || end == text || *end != '\0' || parsed > 0xFFFF) {
    return false;
  }
  songId = static_cast<SND_SONGID>(parsed);
  return true;
}

bool writeWavPcm16Stereo(const char* path, const std::vector<std::int16_t>& samples, std::string& error) {
  if (samples.size() > (std::numeric_limits<std::uint32_t>::max() / sizeof(std::int16_t))) {
    error = "WAV output too large";
    return false;
  }
  const std::uint32_t dataSize = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
  const std::uint32_t riffSize = 36u + dataSize;
  FILE* file = std::fopen(path, "wb");
  if (file == nullptr) {
    error = std::string("Failed to open WAV output: ") + path;
    return false;
  }
  auto writeBytes = [file](const void* data, std::size_t size) -> bool {
    return std::fwrite(data, 1, size, file) == size;
  };
  const char riff[4] = {'R', 'I', 'F', 'F'};
  const char wave[4] = {'W', 'A', 'V', 'E'};
  const char fmt[4] = {'f', 'm', 't', ' '};
  const char data[4] = {'d', 'a', 't', 'a'};
  const std::uint32_t fmtSize = 16;
  const std::uint16_t audioFormat = 1;
  const std::uint16_t channels = 2;
  const std::uint32_t sampleRate = 32000;
  const std::uint16_t bitsPerSample = 16;
  const std::uint16_t blockAlign = static_cast<std::uint16_t>(channels * (bitsPerSample / 8));
  const std::uint32_t byteRate = sampleRate * blockAlign;
  const bool ok = writeBytes(riff, sizeof(riff)) && writeBytes(&riffSize, sizeof(riffSize)) &&
                  writeBytes(wave, sizeof(wave)) && writeBytes(fmt, sizeof(fmt)) &&
                  writeBytes(&fmtSize, sizeof(fmtSize)) && writeBytes(&audioFormat, sizeof(audioFormat)) &&
                  writeBytes(&channels, sizeof(channels)) && writeBytes(&sampleRate, sizeof(sampleRate)) &&
                  writeBytes(&byteRate, sizeof(byteRate)) && writeBytes(&blockAlign, sizeof(blockAlign)) &&
                  writeBytes(&bitsPerSample, sizeof(bitsPerSample)) && writeBytes(data, sizeof(data)) &&
                  writeBytes(&dataSize, sizeof(dataSize)) &&
                  (samples.empty() || writeBytes(samples.data(), samples.size() * sizeof(std::int16_t)));
  std::fclose(file);
  if (!ok) {
    error = std::string("Failed to write WAV output: ") + path;
    return false;
  }
  return true;
}

#if MUSY_TARGET == MUSY_TARGET_PC
extern "C" void salCallback();
#endif

} // namespace

int main(int argc, char* argv[]) {
  if (argc < 5 || argc > 8) {
    printUsage(argv[0]);
    return 1;
  }

  const std::string songPath = argc >= 6 ? argv[5] : defaultSongPath(argv[1]);
  SND_SONGID songId = 0;
  std::string wavPath = defaultWavPath(songPath);
  if (argc == 7) {
    if (!parseSongId(argv[6], songId)) {
      wavPath = argv[6];
    }
  } else if (argc == 8) {
    if (!parseSongId(argv[6], songId)) {
      std::fprintf(stderr, "Invalid song-id: %s\n", argv[6]);
      return 1;
    }
    wavPath = argv[7];
  }

  std::string error;
  std::vector<std::uint8_t> projData;
  std::vector<std::uint8_t> poolData;
  std::vector<std::uint8_t> sampData;
  std::vector<std::uint8_t> sdirData;
  std::vector<std::uint8_t> songData;
  if (!loadFile(argv[1], projData, error) || !loadFile(argv[2], poolData, error) || !loadFile(argv[3], sampData, error) ||
      !loadFile(argv[4], sdirData, error) || !loadFile(songPath.c_str(), songData, error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }

  SND_HOOKS hooks = {std::malloc, std::free};
  sndSetHooks(&hooks);

  if (sndInit(64, 64, 64, 8, SND_FLAGS_DEFAULT, 1234) != 0) {
    std::fprintf(stderr, "sndInit failed\n");
    return 1;
  }

  sndOutputMode(SND_OUTPUTMODE_STEREO);
  sndMasterVolume(127, 0, 1, 1);

  std::unique_ptr<void, decltype(&std::free)> convertedSdir(sndConvert32BitSDIRTo64BitSDIR(sdirData.data()),
                                                            &std::free);
  if (convertedSdir == nullptr) {
    std::fprintf(stderr, "sndConvert32BitSDIRTo64BitSDIR failed\n");
    sndQuit();
    return 1;
  }

  if (!sndPushGroup(projData.data(), kGroupId, sampData.data(), convertedSdir.get(), poolData.data())) {
    std::fprintf(stderr, "sndPushGroup failed\n");
    sndQuit();
    return 1;
  }

  SND_PLAYPARA para{};
  para.flags = SND_PLAYPARA_VOLUME;
  para.volume.target = 127;
  para.volume.time = 0;
  const SND_SEQID seqId = sndSeqPlay(kGroupId, songId, songData.data(), &para);
  if (seqId == SND_SEQ_ERROR_ID) {
    std::fprintf(stderr, "sndSeqPlay failed\n");
    sndQuit();
    return 1;
  }

  sndSeqLoop(seqId, false);
  std::vector<std::int16_t> pcmData;
  constexpr std::size_t kDmaBytesPerFrame = 0x280;
  constexpr std::size_t kDmaSamplesPerFrame = kDmaBytesPerFrame / sizeof(std::int16_t);
  constexpr std::size_t kEstimatedFrameCount = 512;
  pcmData.reserve(kDmaSamplesPerFrame * kEstimatedFrameCount);
#if MUSY_TARGET == MUSY_TARGET_PC
  while (sndSeqGetValid(seqId) || !sndIsIdle()) {
    salCallback();
    if (void* frame = salAiGetDest()) {
      const auto* samples = static_cast<const std::int16_t*>(frame);
      pcmData.insert(pcmData.end(), samples, samples + kDmaSamplesPerFrame);
    }
  }
#else
  while (sndSeqGetValid(seqId) || !sndIsIdle()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
#endif

  if (!writeWavPcm16Stereo(wavPath.c_str(), pcmData, error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    sndQuit();
    return 1;
  }

  sndQuit();
  return 0;
}
