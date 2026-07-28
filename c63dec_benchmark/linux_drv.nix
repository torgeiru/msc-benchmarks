{
  includeos_path ? import ../includeos_source.nix
}
:
let
  includeos = import includeos_path {};
  pkgsStaticStdenv = includeos.pkgs.pkgsStatic.stdenv;
  pkgsMuslStdenv = includeos.pkgs.pkgsMusl.stdenv;
in
# pkgsStaticStdenv.mkDerivation {
pkgsMuslStdenv.mkDerivation {
  name = "VirtioFS_bench Linux";
  version = "dev";
  src = ./src;

  inherit (includeos) nativeBuildInputs;

  cmakeFlags = [
    "-DCMAKE_CXX_STANDARD=20"
  ];

  installPhase = ''
    mkdir -p $out/bin
    cp virtiofs_bench $out/bin/
  '';
}
