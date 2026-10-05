# The libraries the project, its tests and its benchmarks are built against.
#
# This is the one place they are chosen. The Nix package (package.nix) and
# the Bazel build (through bazel.nix) both take them from here, at the
# versions flake.lock pins, so the binary that is developed and benchmarked
# is built from the same libraries as the one that is deployed.
pkgs:
let
  abseil-cpp = pkgs.abseil-cpp.override { cxxStandard = "20"; };
in
{
  # Built as C++20 like the project, so that the two agree on which standard
  # library types Abseil's own types stand for.
  inherit abseil-cpp;

  # io_uring, the faster of the two ways the network layer does I/O.
  liburing = pkgs.liburing;

  # TLS for the connections to Discord.
  openssl = pkgs.openssl;

  # Where the GMs are kept.
  sqlite = pkgs.sqlite;

  # For tests and benchmarks only.
  gtest = pkgs.gtest;
  gbenchmark = pkgs.gbenchmark;

  # Fuzz tests. Not in nixpkgs, so built here (see fuzztest.nix), with the
  # project's compiler and against the Abseil and GoogleTest above. RE2, which
  # it uses for its regular-expression domains, has to be built against that
  # Abseil too.
  fuzztest = pkgs.callPackage ./fuzztest.nix {
    stdenv = pkgs.llvmPackages_21.stdenv;
    inherit abseil-cpp;
    gtest = pkgs.gtest;
    re2 = pkgs.re2.override { inherit abseil-cpp; };
  };
}
