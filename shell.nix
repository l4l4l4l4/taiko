{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  shellHook = ''zsh'';
  buildInputs = [
    pkgs.pkgsCross.avr.buildPackages.gcc
    pkgs.avrdude
  ];
}

