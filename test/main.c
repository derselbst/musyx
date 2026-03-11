#include <musyx/musyx.h>

#include <musyx/seq.h>

#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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
               "Usage: %s <proj> <pool> <samp> <sdir> [song] [song-id]\n"
               "If [song] is omitted, <proj>.song in the same directory is used.\n",
               exe);
}

std::string defaultSongPath(const char* projPath) {
  std::string songPath(projPath);
  const std::size_t slash = songPath.find_last_of("/\\");
  std::size_t dot = songPath.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
    dot = songPath.size();
  }
  songPath.replace(dot, std::string::npos, ".song");
  return songPath;
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

} // namespace

int main(int argc, char* argv[]) {
  if (argc < 5 || argc > 7) {
    printUsage(argv[0]);
    return 1;
  }

  const std::string songPath = argc >= 6 ? argv[5] : defaultSongPath(argv[1]);
  SND_SONGID songId = 0;
  if (argc == 7 && !parseSongId(argv[6], songId)) {
    std::fprintf(stderr, "Invalid song-id: %s\n", argv[6]);
    return 1;
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
  while (sndSeqGetValid(seqId) || !sndIsIdle()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  sndQuit();
  return 0;
}
