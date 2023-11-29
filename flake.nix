{
  outputs = { self, nixpkgs }: let
    pkgs = nixpkgs.legacyPackages.x86_64-linux;
    in {
      packages.x86_64-linux.default = pkgs.stdenv.mkDerivation {
        name = "fastget";
        buildInputs = [ pkgs.fmt pkgs.liburing ];
        nativeBuildInputs = [ pkgs.bear pkgs.clang-tools_16 ];
      };
  };
}
