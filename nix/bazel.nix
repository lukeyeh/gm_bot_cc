# What Bazel sees of deps.nix.
#
# rules_nixpkgs evaluates this file with <nixpkgs> standing for the nixpkgs
# that flake.lock pins, builds the attribute MODULE.bazel asks for, and
# presents the result to Bazel as an external repository.
#
# Nix splits a library into several outputs, typically the libraries in one
# and the headers in another. Bazel wants one directory with both, so each
# library's outputs are merged here.
let
  pkgs = import <nixpkgs> {
    config = { };
    overlays = [ ];
  };
  deps = import ./deps.nix pkgs;

  merged =
    name: pkg:
    pkgs.symlinkJoin {
      inherit name;
      paths = map (output: pkg.${output}) (
        builtins.filter (output: output == "out" || output == "dev") pkg.outputs
      );
    };
in
builtins.mapAttrs merged deps
