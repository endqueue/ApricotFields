// aprifuzz_replay - runs one fuzz target over files, without libFuzzer.
//
// Linked with one target's LLVMFuzzerTestOneInput, it takes directories
// (and files) on its command line and feeds the target every regular file
// in each. A directory that doesn't exist is passed over, so a target with
// no regression yet needs none. A failed check in the target aborts, which
// ctest reports.
//
// This is how each crashing input the fuzzer found stays a test on every
// platform and compiler, libFuzzer or not (CMakeLists.txt registers one per
// target, over tests/fuzz/corpus/<target> and tests/fuzz/regressions/<target>).

#include "aprifuzz.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

static size_t replayed = 0;

// Feeds the target one file; false when it can't be read.
static bool replay_file(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        fprintf(stderr, "aprifuzz_replay: can't open %s\n", path.string().c_str());
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad())
    {
        fprintf(stderr, "aprifuzz_replay: can't read %s\n", path.string().c_str());
        return false;
    }
    // In a block of exactly its size, as libFuzzer hands an input over, so
    // a read past the end is AddressSanitizer's to see.
    uint8_t *exact = (uint8_t *)malloc(bytes.empty() ? 1 : bytes.size());
    if (!exact)
        abort();
    std::copy(bytes.begin(), bytes.end(), exact);
    LLVMFuzzerTestOneInput(exact, bytes.size());
    free(exact);
    replayed++;
    return true;
}

static bool replay_path(const std::filesystem::path &path)
{
    std::error_code error;
    if (!std::filesystem::exists(path, error))
        return true; // not there: nothing to replay
    if (!std::filesystem::is_directory(path, error))
        return replay_file(path);
    // In name order, so a failure names the same file on every system.
    std::vector<std::filesystem::path> files;
    for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(path, error))
    {
        std::string name = entry.path().filename().string();
        if (name.empty() || name[0] == '.')
            continue; // hidden files such as .DS_Store
        if (entry.is_regular_file(error))
            files.push_back(entry.path());
    }
    if (error)
    {
        fprintf(stderr, "aprifuzz_replay: can't list %s\n", path.string().c_str());
        return false;
    }
    std::sort(files.begin(), files.end());
    bool ok = true;
    for (const std::filesystem::path &file : files)
        ok = replay_file(file) && ok;
    return ok;
}

int main(int argc, char **argv)
{
    bool ok = true;
    for (int i = 1; i < argc; i++)
        ok = replay_path(argv[i]) && ok;
    printf("aprifuzz_replay: %zu inputs\n", replayed);
    if (replayed == 0)
    {
        fprintf(stderr, "aprifuzz_replay: no input found\n");
        return 1;
    }
    return ok ? 0 : 1;
}
