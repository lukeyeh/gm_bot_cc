{
  description = "gm_bot: a Discord bot that tracks daily GM streaks";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs?ref=nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

      # The LLVM version everything is compiled with. The exact compiler is
      # whatever flake.lock pins for this major version.
      llvmFor = pkgs: pkgs.llvmPackages_21;
    in
    {
      # The program, for deployment: `nix build`, `nix run`.
      packages = forAllSystems (pkgs: {
        default = pkgs.callPackage ./nix/package.nix {
          stdenv = (llvmFor pkgs).stdenv;
          # The same libraries Bazel builds against.
          inherit (import ./nix/deps.nix pkgs) abseil-cpp liburing openssl sqlite;
        };
      });

      # `nix flake check` builds the package.
      checks = forAllSystems (pkgs: {
        package = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
      });

      devShells = forAllSystems (pkgs:
        let
          # The compiler Bazel builds with. Bazel picks up $CC from this shell.
          llvm = llvmFor pkgs;
        in
        {
          default = (pkgs.mkShell.override { stdenv = llvm.stdenv; }) {
            packages = [
              # nixpkgs' plain `bazel` is an older major version.
              pkgs.bazel_9
              # buildifier, the BUILD file formatter.
              pkgs.bazel-buildtools
              # clangd, clang-format and clang-tidy, matching the compiler.
              llvm.clang-tools
              # The sqlite3 shell, for looking inside gm.db.
              pkgs.sqlite
              # Profiling: perf samples a running program, and flamegraph
              # (from cargo-flamegraph) runs perf and draws the result.
              pkgs.perf
              pkgs.cargo-flamegraph
              # samply opens perf's recordings in the Firefox Profiler, an
              # interactive viewer that runs in the browser.
              pkgs.samply
              # pprof is an alternative viewer, also usable in the terminal.
              # perf_data_converter lets it read perf's recordings, and
              # graphviz draws its call graphs.
              pkgs.pprof
              pkgs.perf_data_converter
              pkgs.graphviz
            ];
          };
        });
    };
}
