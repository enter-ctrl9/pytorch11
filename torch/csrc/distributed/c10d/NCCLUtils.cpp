#include <c10d/NCCLUtils.hpp>

#ifdef USE_C10D_NCCL

#include <mutex>

namespace c10d {

std::string getNcclVersion() {
  static std::once_flag ncclGetVersionFlag;
  static std::string versionString;

  std::call_once(ncclGetVersionFlag, []() {
    int version;
    ncclResult_t status = ncclGetVersion(&version);
    // can't compute the version if call did not return successfully or version
    // code < 100 (corresponding to 0.1.0)
    if (status != ncclSuccess || version < 100) {
      versionString = "Unknown NCCL version";
    } else {
      auto ncclMajor = version / 1000;
      auto ncclMinor = (version % 1000) / 100;
      auto ncclPatch = version % (ncclMajor * 1000 + ncclMinor * 100);
      // this condition is meant to mirror what is done in upstream NCCL
      // e.g., 2.8.0 -> 2800, 2.9.0 -> 20900
      // note that ncclMajor isn't expected to be 3 with the new format,
      // but will be 30 until it is parsed correctly; 3 is used for clarity
      if (ncclMajor >= 3 || (ncclMajor >= 2 && ncclMinor >= 9)) {
        ncclMajor = version / 10000;
        ncclMinor = (version % 10000) / 100;
        ncclPatch = version % (ncclMajor * 10000 + ncclMinor * 100);
      }
      versionString = std::to_string(ncclMajor) + "." +
          std::to_string(ncclMinor) + "." + std::to_string(ncclPatch);
    }
  });

  return versionString;
}

std::string ncclGetErrorWithVersion(ncclResult_t error) {
  return std::string(ncclGetErrorString(error)) + ", NCCL version " +
      getNcclVersion();
}

} // namespace c10d

#endif // USE_C10D_NCCL
