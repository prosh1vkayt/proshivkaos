{ pkgs ? import <nixpkgs> {} }:

# Используем stdenv от кросс-компилятора для aarch64
pkgs.pkgsCross.aarch64-multiplatform.stdenv.mkDerivation {
  name = "arm64-dev-shell";
  
  # Пакеты, необходимые для сборки на хосте (x86_64)
  nativeBuildInputs = with pkgs; [ 
    gnumake 
    # Сюда можно добавить другие утилиты, например: bison, flex, qemu
  ];
}
