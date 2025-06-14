# run with 'nix develop --impure'
# that's because we use builtins.currentSystem. it's not pure :CCC . https://blog.kubukoz.com/flakes-first-steps/
{
  inputs.nixpkgs.url = "github:nixos/nixpkgs";

  outputs = { nixpkgs, ... }:
    let
      system = builtins.currentSystem;
      pkgs = import nixpkgs { inherit system; };
    in
    {
      devShells.${system}.default = pkgs.mkShell {
        packages = [ pkgs.rustc pkgs.cargo ];
        shellHook = "zsh";
      };
    };
}
