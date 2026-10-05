# The bot as a Nix package, for deployment.
#
# This compiles the same sources as `bazel build //:gm_bot`, against the same
# libraries: both this package and Bazel take them from deps.nix. Only the
# compiling itself is done twice over, here by Nix and in development by
# Bazel, because Bazel inside a Nix build is fragile (it wants to download
# things, which a Nix build may not).
#
# What the two builds must still agree on is the compiler flags, below and in
# .bazelrc. Source files need no upkeep: every non-test .cc file under the
# bot's directories is compiled.
{
  lib,
  stdenv,
  abseil-cpp,
  liburing,
  openssl,
  sqlite,
  pkg-config,
}:

stdenv.mkDerivation {
  pname = "gm-bot";
  version = "0.1.0";

  # Only the bot's sources, so that editing a README or a BUILD file does not
  # rebuild the package.
  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../main.cc
      ../async
      ../os
      ../net
      ../http
      ../websocket
      ../json
      ../sqlite
      ../discord
      ../gm
      ../bot
    ];
  };

  nativeBuildInputs = [ pkg-config ];
  buildInputs = [
    abseil-cpp
    liburing
    openssl
    sqlite
  ];

  buildPhase = ''
    runHook preBuild

    sources=$(find main.cc async os net http websocket json sqlite discord gm bot \
                -name '*.cc' ! -name '*_test.cc')
    # The pkg-config names of the libraries the bot uses. Abseil has one per
    # component.
    libraries="absl_flags absl_flags_parse absl_log absl_log_flags
               absl_log_globals absl_log_initialize absl_check absl_status
               absl_statusor absl_status_macros absl_strings absl_str_format
               absl_flat_hash_map absl_flat_hash_set absl_cleanup
               absl_random_random absl_time
               absl_function_ref liburing openssl sqlite3"

    # The flags mirror .bazelrc: C++20, no exceptions.
    $CXX -std=c++20 -O2 -fno-exceptions \
      -Wno-coroutine-missing-unhandled-exception \
      -I. $sources -o gm-bot \
      $(pkg-config --cflags --libs $libraries)

    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 gm-bot $out/bin/gm-bot
    runHook postInstall
  '';

  meta = {
    description = "A Discord bot that tracks daily GM streaks";
    mainProgram = "gm-bot";
    platforms = lib.platforms.linux;
  };
}
