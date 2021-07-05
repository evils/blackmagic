{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  nativeBuildInputs = with pkgs; [
    python3
    python3.pkgs.pyusb
    gcc-arm-embedded
    stlink
    openocd
    dfu-util
  ];
}
