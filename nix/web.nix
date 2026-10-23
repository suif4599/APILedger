{
  lib,
  stdenv,
  emscripten,
  cmake,
  ninja,
  python3,
}:

stdenv.mkDerivation {
  pname = "apiledger-web";
  version = "0.1.0";

  src = lib.cleanSource ../.;

  nativeBuildInputs = [ emscripten cmake ninja python3 ];

  dontUseCmakeConfigure = true;

  configurePhase = ''
    runHook preConfigure
    export HOME=$NIX_BUILD_TOP
    export EM_CACHE=$NIX_BUILD_TOP/emcache
    mkdir -p "$EM_CACHE"
    emcmake cmake -S . -B build -G Ninja \
      -DAPILEDGER_WASM=ON \
      -DCMAKE_BUILD_TYPE=MinSizeRel \
      -DCMAKE_INSTALL_PREFIX=$out
    runHook postConfigure
  '';

  buildPhase = ''
    runHook preBuild
    cmake --build build -j$NIX_BUILD_CORES
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p $out/vendor
    cp web/index.html web/styles.css web/app.js web/report.js $out/
    cp web/vendor/echarts.min.js web/vendor/LICENSE.echarts $out/vendor/
    cp build/apiledger.js build/apiledger.wasm $out/
    runHook postInstall
  '';

  meta = {
    description = "APILedger browser module";
    platforms = lib.platforms.linux;
  };
}
