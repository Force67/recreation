{
  description = "recreation: bethesda content on a modern vulkan engine";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

    # Same pins as the FetchContent declarations in CMakeLists.txt, so
    # `nix build` works inside the sandbox without network access.
    vulkan-headers-src = {
      url = "github:KhronosGroup/Vulkan-Headers/v1.4.304";
      flake = false;
    };
    volk-src = {
      url = "github:zeux/volk/1.4.304";
      flake = false;
    };
    vma-src = {
      url = "github:GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/v3.1.0";
      flake = false;
    };

    # Sibling checkout on the devel branch, vendored equilibrium included.
    # A path input so local fixes land in the sandbox without a commit.
    zetanet-src = {
      url = "path:/home/captainspark/Documents/Projects/zetanet";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, vulkan-headers-src, volk-src, vma-src, zetanet-src }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems
        (system: f nixpkgs.legacyPackages.${system});

      fetchContentFlags = [
        "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"
        "-DFETCHCONTENT_SOURCE_DIR_VULKANHEADERS=${vulkan-headers-src}"
        "-DFETCHCONTENT_SOURCE_DIR_VOLK=${volk-src}"
        "-DFETCHCONTENT_SOURCE_DIR_VULKANMEMORYALLOCATOR=${vma-src}"
      ];
    in
    {
      packages = forAllSystems (pkgs: {
        default = pkgs.stdenv.mkDerivation {
          pname = "recreation";
          version = "0.1.0";
          src = self;

          nativeBuildInputs = with pkgs; [ cmake ninja glslang ];
          buildInputs = with pkgs; [ sdl3 openssl ];

          cmakeFlags = fetchContentFlags ++ [
            "-DRECREATION_ZETANET_DIR=${zetanet-src}"
          ];

          # No install rules yet, the binaries are the product.
          installPhase = ''
            runHook preInstall
            mkdir -p $out/bin
            cp runtime/recreation $out/bin/
            cp runtime/recreation-server $out/bin/
            cp tools/esminfo/esminfo $out/bin/
            runHook postInstall
          '';

          meta.mainProgram = "recreation";
        };
      });

      devShells = forAllSystems (pkgs:
        let
          # Launcher for nix-built vulkan binaries. volk dlopens
          # libvulkan.so.1, which is never linked, so the loader has to be
          # on the search path; the host GPU driver (the ICD, e.g.
          # libGLX_nvidia) lives in the system lib dir and was built
          # against the host glibc, so nix's glibc and libstdc++ go first
          # and everything resolves against the newer ones. Only the
          # driver's own libraries are exposed, through a symlink dir:
          # putting the whole host lib dir on LD_LIBRARY_PATH shadows nix
          # libraries (SDL's libxkbcommon, etc.) with older host ones.
          # This must not leak into the whole shell either: host binaries
          # run with the host ld.so and crash when handed nix's libc.
          vkrun = pkgs.writeShellScriptBin "vkrun" ''
            driver_libs="''${XDG_RUNTIME_DIR:-/tmp}/recreation-driver-libs"
            mkdir -p "$driver_libs"
            for f in /usr/lib/*-linux-gnu/libnvidia*.so* /usr/lib/*-linux-gnu/libGLX_nvidia.so* \
                     /usr/lib/*-linux-gnu/libEGL_nvidia.so*; do
              [ -e "$f" ] && ln -sf "$f" "$driver_libs/"
            done
            if [ -e /usr/share/vulkan/icd.d/nvidia_icd.json ]; then
              export VK_DRIVER_FILES=''${VK_DRIVER_FILES:-/usr/share/vulkan/icd.d/nvidia_icd.json}
            fi
            # X client libs, libglvnd (the driver dlopens libEGL.so.1
            # during init) and libbsd are needed by libGLX_nvidia and ABI
            # stable, nix's copies serve both worlds.
            export LD_LIBRARY_PATH=${pkgs.lib.makeLibraryPath [
              pkgs.glibc
              pkgs.stdenv.cc.cc.lib
              pkgs.vulkan-loader
              pkgs.libglvnd
              pkgs.libx11
              pkgs.libxext
              pkgs.libxcb
              pkgs.libxau
              pkgs.libxdmcp
              pkgs.libbsd
              pkgs.libmd
            ]}:"$driver_libs"''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
            exec "$@"
          '';

          # Full software path: an Xvfb display plus mesa's lavapipe ICD.
          # The GB10 driver has no headless surface and its X11 WSI needs
          # the nvidia X driver, so windowed runs on a virtual display go
          # through lavapipe. CPU rendering, but it executes the entire
          # frame for validation.
          swrun = pkgs.writeShellScriptBin "swrun" ''
            ${pkgs.xorg.xvfb}/bin/Xvfb :99 -screen 0 1280x720x24 &
            xvfb_pid=$!
            trap 'kill $xvfb_pid 2>/dev/null' EXIT
            sleep 0.5
            export DISPLAY=:99
            export VK_DRIVER_FILES=${pkgs.mesa}/share/vulkan/icd.d/lvp_icd.${pkgs.stdenv.hostPlatform.uname.processor}.json
            # lavapipe's threaded acceleration structure builds race and
            # segfault (observed on aarch64, mesa 25 / llvm 21); a single
            # rasterizer thread is slow but deterministic.
            export LP_NUM_THREADS=''${LP_NUM_THREADS:-1}
            export LD_LIBRARY_PATH=${pkgs.lib.makeLibraryPath [
              pkgs.vulkan-loader
            ]}''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
            "$@"
          '';
        in
        {
          default = pkgs.mkShell {
            packages = with pkgs; [
              cmake
              ninja
              gdb
              glslang
              sdl3
              openssl  # zetanet crypto backend
              vulkan-headers
              vulkan-loader
              vulkan-validation-layers
              vulkan-tools
              vkrun
              swrun
            ];

            shellHook = ''
              export VK_LAYER_PATH=${pkgs.vulkan-validation-layers}/share/vulkan/explicit_layer.d
              export RECREATION_FETCHCONTENT_FLAGS="${builtins.concatStringsSep " " fetchContentFlags}"

              if [ -z "''${DISPLAY:-}" ] && [ -S /tmp/.X11-unix/X0 ]; then
                export DISPLAY=:0
              fi
            '';
          };
        });
    };
}
