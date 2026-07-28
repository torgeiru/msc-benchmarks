{
  includeos_path ? import ./includeos_source.nix,
  use_patched_virtiofsd ? false,
}
:
let
  includeos = import includeos_path {};
  stdenv = includeos.stdenv;
  pkgs = includeos.pkgs;
  patchedVirtiofsd = pkgs.virtiofsd.overrideAttrs (old: {
    patches = (old.patches or []) ++ [ ./virtiofsd-poll.patch ];
  });
  virtiofsd = if use_patched_virtiofsd then patchedVirtiofsd else pkgs.virtiofsd;
in
pkgs.mkShell.override { inherit (includeos) stdenv; } {
  packages = [
    includeos.vmrunner
    stdenv.cc
    pkgs.buildPackages.cmake
    pkgs.buildPackages.nasm
    pkgs.qemu
    virtiofsd
    pkgs.which
    pkgs.grub2
    pkgs.iputils
  ];

  buildInputs = [
    includeos
    includeos.chainloader
  ];

  bootloader="${includeos}/boot/bootloader";
}
