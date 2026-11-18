{
  description = "Fast-evaluating git-loom and U-Boot verification tooling flake";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
    git-loom-src = {
      url = "github:narnaud/git-loom/v1.1.0";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, git-loom-src }:
    let
      supportedSystems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs supportedSystems;
    in
    {
      packages = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          git-loom = pkgs.rustPlatform.buildRustPackage {
            pname = "git-loom";
            version = "1.1.0";
            src = git-loom-src;
            cargoLock.lockFile = "${git-loom-src}/Cargo.lock";
            nativeBuildInputs = [ pkgs.pkg-config pkgs.git ];
            buildInputs = [ pkgs.openssl pkgs.zlib ];
            doCheck = false;
            meta = {
              description = "Weave stacked Git branches into an integration branch";
              homepage = "https://github.com/narnaud/git-loom";
              mainProgram = "git-loom";
            };
          };
        in
        {
          inherit git-loom;
          default = git-loom;
        });

      checks = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          git-loom = self.packages.${system}.git-loom;
        in
        {
          loom-tests = pkgs.runCommand "uboot-loom-tests"
            {
              nativeBuildInputs = [
                git-loom
                pkgs.git
                pkgs.python3
              ];
            }
            ''
              export HOME="$TMPDIR/home"
              mkdir -p "$HOME" "$TMPDIR/loom"
              cp ${self}/weave.sh ${self}/test-weave.sh ${self}/loom.toml "$TMPDIR/loom/"
              chmod -R +w "$TMPDIR/loom"
              patchShebangs "$TMPDIR/loom"
              "$TMPDIR/loom/test-weave.sh"
              python3 -c '
              import tomllib
              with open("${self}/loom.toml", "rb") as f:
                  cfg = tomllib.load(f)
              assert cfg.get("base"), "missing base"
              assert len(cfg.get("branch", [])) >= 15, "expected active feature branches"
              print("Manifest validated:", len(cfg["branch"]), "branches on", cfg["base"])
              '
              touch "$out"
            '';
        });

      devShells = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          ubootPython = pkgs.python3.withPackages (p: [
            p.libfdt
            p.pyelftools
            p.setuptools
          ]);
        in
        {
          default = pkgs.mkShell {
            NIX_HARDENING_ENABLE = "";
            packages = with pkgs; [
              self.packages.${system}.git-loom
              b4
              bc
              bison
              dtc
              flex
              git
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
          };
        });
    };
}
