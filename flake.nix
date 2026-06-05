{
  description = "Development environment for fossil";

  inputs = {
    nixpkgs.url     = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    pmdk-src = {
      url = "github:daos-stack/pmdk";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, flake-utils, pmdk-src, ... }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
        llvm = pkgs.llvmPackages_latest;
        pmdk = pkgs.stdenv.mkDerivation {
          pname = "pmdk";
          version = "git";
          src = pmdk-src;

          enableParallelBuilding = true;
          dontConfigure = true;
          dontUseCmakeConfigure = true;
          postPatch = ''
            patchShebangs .
          '';

          nativeBuildInputs = [
            pkgs.autoconf
            pkgs.automake
            pkgs.pkg-config
            pkgs.perl
            pkgs.which
          ];

          buildInputs = [
            pkgs.glib
            pkgs.libfabric
            pkgs.ncurses
            pkgs.pandoc
          ];

          makeFlags = [
            "prefix=${placeholder "out"}"
            "NDCTL_ENABLE=n"
            "PMEMOBJ_IGNORE_DIRTY_SHUTDOWN=y"
            "PMEMOBJ_IGNORE_BAD_BLOCKS=y"
          ];

          buildPhase = ''
            runHook preBuild
            make -C src prefix="$out" NDCTL_ENABLE=n PMEMOBJ_IGNORE_DIRTY_SHUTDOWN=y PMEMOBJ_IGNORE_BAD_BLOCKS=y
            runHook postBuild
          '';

          installFlags = [
            "prefix=${placeholder "out"}"
            "NDCTL_ENABLE=n"
            "PMEMOBJ_IGNORE_DIRTY_SHUTDOWN=y"
            "PMEMOBJ_IGNORE_BAD_BLOCKS=y"
          ];

          installPhase = ''
            runHook preInstall
            make -C src install prefix="$out" NDCTL_ENABLE=n PMEMOBJ_IGNORE_DIRTY_SHUTDOWN=y PMEMOBJ_IGNORE_BAD_BLOCKS=y
            runHook postInstall
          '';
        };
      in {
        packages.pmdk = pmdk;

        devShells.default = pkgs.mkShell.override { stdenv = llvm.stdenv; } {

          nativeBuildInputs = [
            llvm.clang-tools
            llvm.clang
            llvm.llvm
            pkgs.cmake
            pkgs.gdb
            pkgs.pkg-config
          ];

          buildInputs = [
            llvm.libcxx
            pkgs.db
            pkgs.leveldb
            pmdk
          ];

          packages = [
            (pkgs.python3.withPackages (ps: with ps; [
              pip
              setuptools
              numpy
              matplotlib
            ]))
            pkgs.ripgrep
            pkgs.snappy.dev
          ];

          shellHook = ''
            export CXXFLAGS="$NIX_CFLAGS_COMPILE"
          '';
        };
      });
}
