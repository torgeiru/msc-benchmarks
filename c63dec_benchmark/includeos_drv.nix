{
    includeos_path ? import ../includeos_source.nix,
    use_interrupts ? false
}
:
let
    includeos = import includeos_path {};
    stdenv = includeos.stdenv;
    virtiofs_driver =
      if use_interrupts then "virtiofs_interrupts" else "virtiofs_polling";
in
stdenv.mkDerivation {
    name = "VirtioFS_bench IncludeOS";
    version = "dev";
    src = ./src;

    inherit (includeos) nativeBuildInputs;

    cmakeFlags = [
      "-DUNIKERNEL=1"
      "-DVIRTIOFS_DRIVER=${virtiofs_driver}"
    ];

    buildInputs = [
        includeos
    ];

    installPhase = ''
        mkdir -p $out/bin
        cp virtiofs_bench.elf.bin $out/bin/virtiofs_bench
    '';
}
