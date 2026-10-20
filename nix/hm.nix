{ config, lib, pkgs, ... }:

let
  cfg = config.programs.apiledger;
in
{
  options.programs.apiledger = {
    enable = lib.mkEnableOption "APILedger";

    package = lib.mkOption {
      type = lib.types.package;
      default = pkgs.callPackage ./package.nix { };
      defaultText = lib.literalExpression "pkgs.callPackage ./package.nix { }";
      description = "APILedger package to install.";
    };

    dataDir = lib.mkOption {
      type = lib.types.str;
      default = "${config.xdg.dataHome}/apiledger";
      description = ''
        Directory holding the SQLite ledger, the default report, the ECharts
        cache, and user presets under `presets/`.
      '';
    };

    presets = lib.mkOption {
      type = lib.types.nullOr lib.types.path;
      default = null;
      description = ''
        Directory of extra JSON presets. Its contents are linked to
        `<dataDir>/presets`; files with the same name override bundled presets.
      '';
    };
  };

  config = lib.mkIf cfg.enable {
    home.packages = [ cfg.package ];
    home.sessionVariables.APILEDGER_ROOT = cfg.dataDir;
    home.file = lib.mkIf (cfg.presets != null) {
      "${cfg.dataDir}/presets".source = cfg.presets;
    };
  };
}
