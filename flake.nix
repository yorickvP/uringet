{
  outputs = { self, nixpkgs }: let
    pkgs = nixpkgs.legacyPackages.x86_64-linux;
    pkgsStatic = nixpkgs.legacyPackages.x86_64-linux.pkgsStatic;
    in {
      packages.x86_64-linux.default = pkgsStatic.stdenv.mkDerivation {
        name = "fastget";
        src = ./.;
        buildInputs = with pkgsStatic; [ fmt (liburing.overrideAttrs (o: {
          dontAddStaticConfigureFlags = true;
          configureFlags = [];
          makeFlags = [ "ENABLE_SHARED=0" ];
        })) libexecinfo ];
        installPhase = "mkdir -p $out/bin && cp ./main $out/bin/fastget";
        nativeBuildInputs = [ pkgs.bear pkgs.clang-tools_16 ];
      };
  };
}
