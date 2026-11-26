#include <cstdio>
#include <string>

#include "core/result.h"
#include "tools/net-probe/net_probe.h"

int main(int argc, char* argv[]) {
  const blue::core::Result<blue::netprobe::ProbeOptions, std::string> options =
      blue::netprobe::ParseOptions(argc, argv);
  if (!options.ok()) {
    std::printf("error: %s\n%s\n", options.error().c_str(),
                blue::netprobe::kUsage);
    return 2;
  }
  if (options.value().show_help) {
    std::printf("%s\n", blue::netprobe::kUsage);
    return 0;
  }
  return blue::netprobe::RunProbe(options.value());
}
