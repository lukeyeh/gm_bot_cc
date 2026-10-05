#include "absl/base/log_severity.h"
#include "absl/flags/parse.h"
#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "bot/bot.h"
#include "bot/config.h"

namespace {

// Runs the bot until it stops; never returns OK.
absl::Status Run() {
  ABSL_ASSIGN_OR_RETURN(const bot::Config config, bot::LoadConfig(),
                        _.SetPrepend() << "configuration: ");

  return bot::Run(config);
}

}  // namespace

int main(int argc, char** argv) {
  // The bot's own settings are environment variables (see bot/config.h).
  // Flags are for the libraries: --io_backend, and Abseil's logging flags.
  absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();
  absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfo);

  LOG(ERROR) << Run();
  return 1;
}
