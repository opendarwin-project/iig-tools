{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }: let
    system = "x86_64-linux";
    pkgs = import nixpkgs { inherit system; };
    iig-tools = pkgs.callPackage ./. {};
  in {
    packages.${system} = {
      inherit iig-tools;
      default = iig-tools;
    };
  };
}
