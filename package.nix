{
  lib,
  stdenv,
  fetchFromGitHub,
  meson,
  ninja,
  pkg-config,
  curl,
  jansson,
  versionCheckHook,
}:

stdenv.mkDerivation (finalAttrs: {
  pname = "hax";
  version = "0.5.0";

  src = fetchFromGitHub {
    owner = "basilgood";
    repo = "hax";
    rev = "master";
    hash = "sha256-Ivs56jvRU/VBUAmcSjWrf89P6JlPiquiMkmr/zGBmnA=";
  };

  nativeBuildInputs = [
    meson
    ninja
    pkg-config
  ];

  buildInputs = [
    curl
    jansson
  ];

  doInstallCheck = true;
  nativeInstallCheckInputs = [ versionCheckHook ];

  meta = {
    description = "Minimalist, terminal-native coding agent written in C";
    homepage = "https://usehax.dev";
    license = lib.licenses.mit;
    mainProgram = "hax";
    platforms = lib.platforms.unix;
  };
})
