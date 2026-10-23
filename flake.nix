{
  description = "APILedger - API bill ledger CLI with a self-contained HTML report";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = function:
        nixpkgs.lib.genAttrs systems
          (system: function nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAllSystems (pkgs: {
        default = pkgs.callPackage ./nix/package.nix { };
        apiledger = pkgs.callPackage ./nix/package.nix { };
        web = pkgs.callPackage ./nix/web.nix { };
      });

      homeManagerModules.default = import ./nix/hm.nix;
      homeManagerModules.apiledger = import ./nix/hm.nix;

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          packages = with pkgs; [ cmake ninja curl nodejs emscripten ];
        };
      });
    };
}
