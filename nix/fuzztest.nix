# FuzzTest (https://github.com/google/fuzztest), which nixpkgs does not
# package, built from source with nixpkgs' tools.
#
# Two things about its CMake build need working around:
#
#  - It downloads Abseil, RE2, GoogleTest and ANTLR as it configures. A Nix
#    build has no network, and the tests must link the same Abseil and
#    GoogleTest as the code they test in any case, so the file that does the
#    downloading is replaced with one that finds the ones passed in here.
#
#  - It has no install step, and builds dozens of small static libraries. The
#    install phase below gathers the headers and merges the libraries into
#    one, so that whoever links it need not know their order.
#
# The grammar-based domains are left out: they are what needs ANTLR, and
# nothing here uses them.
{
  lib,
  stdenv,
  fetchFromGitHub,
  cmake,
  ninja,
  abseil-cpp,
  gtest,
  re2,
}:

stdenv.mkDerivation {
  pname = "fuzztest";
  # FuzzTest makes no releases; this is the date of the commit.
  version = "0-unstable-2026-10-05";

  src = fetchFromGitHub {
    owner = "google";
    repo = "fuzztest";
    rev = "305251260059201f842e5de6bb10e7b73efb1a6e";
    hash = "sha256-6/vHqJuh0NpE+phKun1EeaVruFHjnWUsi7Q9iKr7dWE=";
  };

  nativeBuildInputs = [
    cmake
    ninja
  ];
  buildInputs = [
    abseil-cpp
    gtest
    re2
  ];

  postPatch = ''
    cat > cmake/BuildDependencies.cmake <<'CMAKE'
    find_package(absl REQUIRED)
    find_package(re2 REQUIRED)
    find_package(GTest REQUIRED)
    CMAKE

    sed -i \
      -e '/add_subdirectory(fuzztest\/grammars)/d' \
      -e '/add_subdirectory(grammar_codegen/d' \
      -e '/add_subdirectory(tools)/d' \
      -e 's/set(CMAKE_CXX_STANDARD 17)/set(CMAKE_CXX_STANDARD 20)/' \
      CMakeLists.txt
  '';

  # One of its sources includes a header that RE2 keeps to itself and does
  # not install. RE2's own source tree, at the version being linked, has it.
  env.NIX_CFLAGS_COMPILE = "-I${re2.src}";

  cmakeFlags = [
    "-DBUILD_SHARED_LIBS=OFF"
    "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
  ];

  installPhase = ''
    runHook preInstall

    # Headers, keeping their paths: they include one another by path from
    # the top of the source tree.
    mkdir -p $out/include $out/lib
    (cd .. && find fuzztest common -name '*.h' -exec cp --parents {} $out/include \;)

    # The library that provides main() stays separate, for tests that would
    # rather bring their own.
    main=$(find . -name 'libfuzztest_fuzztest_gtest_main.a')
    cp $main $out/lib/libfuzztest_gtest_main.a

    {
      echo "create $out/lib/libfuzztest.a"
      find . -name '*.a' ! -name 'libfuzztest_fuzztest_gtest_main.a' -printf 'addlib %p\n'
      echo save
      echo end
    } | $AR -M

    # FuzzTest's headers include RE2's, so whoever compiles against the one
    # needs the other, at this version: carry it along.
    ln -s ${lib.getDev re2}/include/re2 $out/include/re2
    ln -s ${lib.getLib re2}/lib/libre2.so.* $out/lib/

    runHook postInstall
  '';

  meta = {
    description = "A C++ testing framework for writing and running fuzz tests";
    homepage = "https://github.com/google/fuzztest";
    license = lib.licenses.asl20;
    platforms = lib.platforms.linux;
  };
}
