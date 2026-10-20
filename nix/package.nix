{
  lib,
  stdenv,
  cmake,
  curl,
}:

stdenv.mkDerivation {
  pname = "apiledger";
  version = "0.1.0";

  src = lib.cleanSource ../.;

  nativeBuildInputs = [ cmake ];
  buildInputs = [ curl ];

  cmakeFlags = [
    "-DAPILEDGER_SHARE_DIR=${placeholder "out"}/share/apiledger"
  ];

  meta = {
    description = "API bill ledger CLI with a self-contained HTML report";
    mainProgram = "apiledger";
    platforms = lib.platforms.linux;
  };
}
