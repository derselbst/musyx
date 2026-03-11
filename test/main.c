#include <musyx/musyx.h>

#include <musyx/seq.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

std::vector<std::uint8_t> loadFile(const char* path) {
  FILE* file = std::fopen(path, "rb");
  if (file == nullptr) {
    throw std::string("Failed to open file: ") + path;
  }

  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::rewind(file);
  if (size <= 0) {
    std::fclose(file);
    throw std::string("File is empty or invalid: ") + path;
  }

  std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
  const std::size_t readSize = std::fread(data.data(), 1, data.size(), file);
  std::fclose(file);
  if (readSize != data.size()) {
    throw std::string("Failed to read file fully: ") + path;
  }
  return data;
}

void printUsage(const char* exe) {
  std::fprintf(stderr,
               "Usage: %s <proj> <pool> <samp> <sdir> [song] [song-id]\n"
               "If [song] is omitted, <proj>.song in the same directory is used.\n",
               exe);
}

} // namespace

int main(int argc, char* argv[]) {
  if (argc < 5 || argc > 7) {
    printUsage(argv[0]);
    return 1;
  }

  try {
    const std::filesystem::path projPath(argv[1]);
    const std::filesystem::path songPath =
        argc >= 6 ? std::filesystem::path(argv[5]) : projPath.parent_path() / (projPath.stem().string() + ".song");
    const SND_SONGID songId = argc == 7 ? static_cast<SND_SONGID>(std::strtoul(argv[6], nullptr, 0)) : 0;

    auto projData = loadFile(argv[1]);
    auto poolData = loadFile(argv[2]);
    auto sampData = loadFile(argv[3]);
    auto sdirData = loadFile(argv[4]);
    auto songData = loadFile(songPath.string().c_str());

    SND_HOOKS hooks = {std::malloc, std::free};
    sndSetHooks(&hooks);

    if (sndInit(64, 64, 64, 8, SND_FLAGS_DEFAULT, 1234) != 0) {
      std::fprintf(stderr, "sndInit failed\n");
      return 1;
    }

    sndOutputMode(SND_OUTPUTMODE_STEREO);
    sndMasterVolume(127, 0, 1, 1);

    void* convertedSdir = sndConvert32BitSDIRTo64BitSDIR(sdirData.data());
    if (convertedSdir == nullptr) {
      std::fprintf(stderr, "sndConvert32BitSDIRTo64BitSDIR failed\n");
      sndQuit();
      return 1;
    }
    sdirData.clear();

    if (!sndPushGroup(projData.data(), 1, sampData.data(), convertedSdir, poolData.data())) {
      std::fprintf(stderr, "sndPushGroup failed\n");
      sndQuit();
      std::free(convertedSdir);
      return 1;
    }

    SND_PLAYPARA para{};
    para.flags = SND_PLAYPARA_VOLUME;
    para.volume.target = 127;
    para.volume.time = 0;
    const SND_SEQID seqId = sndSeqPlay(1, songId, songData.data(), &para);
    if (seqId == SND_SEQ_ERROR_ID) {
      std::fprintf(stderr, "sndSeqPlay failed\n");
      sndQuit();
      std::free(convertedSdir);
      return 1;
    }

    sndSeqLoop(seqId, false);
    while (sndSeqGetValid(seqId) || !sndIsIdle()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    sndQuit();
    std::free(convertedSdir);
    return 0;
  } catch (const std::string& err) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
}
