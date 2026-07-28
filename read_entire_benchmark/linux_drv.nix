{
  includeos_path ? import ../includeos_source.nix
}
:
let
  includeos = import includeos_path {};
  pkgsStatic = includeos.pkgs.pkgsStatic;
  pkgsStaticStdenv = pkgsStatic.stdenv;
in
pkgsStaticStdenv.mkDerivation {
  name = "VirtioFS whole-file read benchmark Linux";
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
