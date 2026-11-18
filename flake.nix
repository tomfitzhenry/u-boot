{
  description = "U-Boot git-loom integration tree and board verification suite";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
    git-loom-src = {
      url = "github:narnaud/git-loom/v1.1.0";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, git-loom-src }:
    let
      loomFlake = (import ./loom/flake.nix).outputs {
        inherit nixpkgs git-loom-src;
        self = ./loom;
      };
      supportedSystems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs supportedSystems;
    in
    {
      inherit (loomFlake) packages devShells;

      checks = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          ubootPython = pkgs.python3.withPackages (p: [
            p.libfdt
            p.pyelftools
            p.setuptools
          ]);
        in
        {
          inherit (loomFlake.checks.${system}) loom-tests;

          uboot-verify = pkgs.stdenv.mkDerivation {
            name = "uboot-loom-verify";
            src = self;
            dontUnpack = true;
            dontConfigure = true;
            dontFixup = true;
            hardeningDisable = [ "all" ];

            nativeBuildInputs = with pkgs; [
              bc
              bison
              dtc
              flex
              gnutls
              ncurses
              openssl
              perl
              pkg-config
              qemu
              swig
              ubootPython
              ubootTools
              util-linux
              which
              xxd
              pkgsCross.aarch64-multiplatform.stdenv.cc
              pkgsCross.armv7l-hf-multiplatform.stdenv.cc
            ];

            buildPhase = ''
              runHook preBuild
              export BOOT_LOG_OUT="$TMPDIR/qemu-spl-boot.log"
              bash "$src/loom/verify-uboot.sh" "$src"
              runHook postBuild
            '';

            installPhase = ''
              mkdir -p "$out"
              cp "$TMPDIR/qemu-spl-boot.log" "$out/qemu-spl-boot.log" 2>/dev/null || true
              touch "$out/passed"
            '';
          };
        });
    };
}
