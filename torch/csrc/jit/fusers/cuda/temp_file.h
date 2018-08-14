#if defined USE_CUDA && !(defined _WIN32) && !(defined USE_ROCM)
#pragma once

#include "torch/csrc/utils/disallow_copy.h"

#include "torch/csrc/jit/assertions.h"

#include "unistd.h"

#include <string>
#include <vector>
#include <cstdio>

namespace torch { namespace jit { namespace cudafuser {

struct TempFile {
  TH_DISALLOW_COPY_AND_ASSIGN(TempFile);

  TempFile(const std::string& t, int suffix) {
    // mkstemps edits its first argument in places
    // so we make a copy of the string here, including null terminator
    std::vector<char> tt(t.c_str(), t.c_str() + t.size() + 1);
    int fd = mkstemps(tt.data(), suffix);
    JIT_ASSERT(fd != -1);
    file_ = fdopen(fd, "r+");

    // - 1 becuase tt.size() includes the null terminator,
    // but std::string does not expect one
    name_ = std::string(tt.begin(), tt.end() - 1);
  }

  void sync() { fflush(file_); }

  void write(const std::string & str) {
    size_t result = fwrite(str.c_str(), 1, str.size(), file_);
    JIT_ASSERT(str.size() == result);
  }

  // Getters
  const std::string& name() const { return name_; }
  
  FILE* file()  { return file_; }

  ~TempFile() {
    if(file_ != nullptr) {
      // unlink first to ensure another mkstemps doesn't
      // race between close and unlink
      unlink(name_.c_str());
      fclose(file_);
    }
  }
private:
  FILE* file_ = nullptr;
  std::string name_;
};

} // namespace cudafuser
} // namespace jit
} // namespace torch

#endif // defined USE_CUDA && !(defined _WIN32) && !(defined USE_ROCM)