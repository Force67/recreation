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

    # The co-developed siblings, at the revisions .github/workflows/build.yml
    # pins: the sandbox and CI then compile the same engine, and moving either
    # one is the same deliberate edit. They are fetched rather than read out of
    # a working copy, so no checkout has to sit next to this one -- at the cost
    # that local edits only reach the sandbox once pushed.
    #
    # `submodules=1` where CI checks out with `submodules: recursive`, which is
    # the flag a github: tarball input cannot carry. Of rx's third_party tree
    # only what git tracks arrives: the FidelityFX/DLSS/NRD/Jolt SDKs are
    # downloads (rx/tools/get_*.sh), so this build turns those features off
    # (physics included) with a line in the configure log. A working copy with
    # the SDKs fetched is still the way to build with them -- point
    # RECREATION_RX_DIR at one.

    # zetanet: the UDP transport and reliability layer, vendored equilibrium
    # included. Its branch is develop, not main.
    zetanet-src = {
      url = "git+https://github.com/Force67/zetanet?submodules=1&ref=develop&rev=80ed00c4c71a4a204d79a82636fa7819f046db18";
      flake = false;
    };

    # nanobuf: the wire-message toolchain. Provides nanoc (the schema
    # compiler) and the header-only C++ runtime.
    nanobuf-src = {
      url = "git+https://github.com/Force67/nanobuf?ref=main&rev=7cb0cd95a7b06ac2fb335924816b7ac8cb6fa092";
      flake = false;
    };

    # rx: the extracted generic engine (core/ecs/asset/render/physics/anim/
    # audio/rpc/ui + the imgui/cgltf/stb vendored libs).
    rx-src = {
      url = "git+https://github.com/Force67/rx?submodules=1&ref=main&rev=26345666e341d3ccf2cf1c631ccde4ec4bac6094";
      flake = false;
    };

    # libultragui: the GPU UI middleware behind the HUD, the menus and (since
    # rx grew an rx::ui module) the engine splash every rx application shows,
    # which is what makes it a hard requirement of any build rather than an
    # optional one.
    libultragui-src = {
      url = "git+https://github.com/Force67/libultragui?submodules=1&ref=main&rev=03a73206ca6f1ec3bef9d7b229c1343d66b3722b";
      flake = false;
    };

    # kinema: the skeletal animation runtime rx links.
    kinema-src = {
      url = "git+https://github.com/Force67/kinema?ref=main&rev=5cee20221bff4eba4027e99068c35261bb45a60b";
      flake = false;
    };

    # equilibrium: base:: containers, strings and atomics. It is a submodule of
    # this repository, and `src = self` is the git tree without submodules, so
    # the sandbox takes it as an input instead -- at the revision the submodule
    # records (git ls-tree HEAD third_party/equilibrium), which is the head of
    # devel5, the branch .gitmodules tracks. A tarball input, not
    # git+submodules: equilibrium's own eight submodules are test and platform
    # deps behind EQ_BUILD_TESTS, and this build reads none of them.
    equilibrium-src = {
      url = "github:Force67/equilibrium/f961dcb8c9965c54c961086144158e043f68a80c";
      flake = false;
    };

    # rx's third_party/NRD-MathLib FetchContent-fetches sse2neon on aarch64;
    # under FETCHCONTENT_FULLY_DISCONNECTED that fetch fails unless we hand it
    # a source dir. Pinned to the master ref NRD-MathLib declares.
    sse2neon-src = {
      url = "github:DLTcollab/sse2neon";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, vulkan-headers-src, volk-src, vma-src, zetanet-src, nanobuf-src, rx-src, libultragui-src, kinema-src, equilibrium-src, sse2neon-src }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems
        (system: f nixpkgs.legacyPackages.${system});

      fetchContentFlags = [
        "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"
        "-DFETCHCONTENT_SOURCE_DIR_VULKANHEADERS=${vulkan-headers-src}"
        "-DFETCHCONTENT_SOURCE_DIR_VOLK=${volk-src}"
        "-DFETCHCONTENT_SOURCE_DIR_VULKANMEMORYALLOCATOR=${vma-src}"
        "-DFETCHCONTENT_SOURCE_DIR_SSE2NEON=${sse2neon-src}"
      ];

      # WineHQ vkd3d (native Linux D3D12-on-Vulkan), built from the release
      # tarball: the git tree needs wine's widl to generate the d3d12 headers,
      # which nixpkgs cannot supply on aarch64, while the dist tarball ships
      # them pregenerated. Provides libvkd3d (the D3D12 implementation),
      # libvkd3d-utils (D3D12CreateDevice & friends) and libvkd3d-shader with
      # the DXIL (SM6) frontend. Used to run the engine's d3d12 backend on
      # this Linux box.
      vkd3dFor = pkgs: pkgs.stdenv.mkDerivation rec {
        pname = "vkd3d";
        version = "2.0";
        src = pkgs.fetchurl {
          url = "https://dl.winehq.org/vkd3d/source/vkd3d-${version}.tar.xz";
          hash = "sha256-mtKbsjaAgYakfsZuhTsh5vylm53GKpR0sF1ePtonEO8=";
        };
        nativeBuildInputs = with pkgs; [ pkg-config bison flex perl perlPackages.JSON ];
        buildInputs = with pkgs; [ spirv-headers vulkan-headers vulkan-loader xorg.libxcb ];
        configureFlags = [ "--disable-tests" "--disable-doxygen-doc" ];
        enableParallelBuilding = true;
      };

      # nanoc, built reproducibly from the pinned nanobuf source. The lockfile
      # drives dependency resolution (no manual cargoHash); the workspace has
      # no git deps and only the `nanoc` binary is built. This goes on PATH in
      # the dev shell so `nanobuf_regen` always has a compiler.
      nanocFor = pkgs: pkgs.rustPlatform.buildRustPackage {
        pname = "nanoc";
        version = "0.1.0";
        src = nanobuf-src;
        cargoLock.lockFile = "${nanobuf-src}/Cargo.lock";
        cargoBuildFlags = [ "-p" "nanoc" ];
        doCheck = false;
        meta.mainProgram = "nanoc";
      };
    in
    {
      packages = forAllSystems (pkgs: {
        nanoc = nanocFor pkgs;

        default = pkgs.stdenv.mkDerivation {
          pname = "recreation";
          version = "0.1.0";
          src = self;

          # dxc compiles the .hlsl shaders, slangc the .slang ones rx's renderer
          # has carried since it took a second shader language.
          nativeBuildInputs = with pkgs; [ cmake ninja directx-shader-compiler shader-slang pkg-config ];
          # ffmpeg supplies libav* for the compressed game audio codecs (xWMA,
          # Wwise, the WMA inside FUZ voice files), enabled below.
          # wayland: libwayland-client for the KDE HDR-toggle monitor (linux).
          buildInputs = with pkgs; [ sdl3 openssl freetype harfbuzz ffmpeg ]
            ++ pkgs.lib.optionals pkgs.stdenv.isLinux [ pkgs.wayland ];

          cmakeFlags = fetchContentFlags ++ [
            "-DRECREATION_ZETANET_DIR=${zetanet-src}"
            "-DRECREATION_NANOBUF_DIR=${nanobuf-src}"
            "-DRECREATION_RX_DIR=${rx-src}"
            "-DRECREATION_LIBULTRAGUI_DIR=${libultragui-src}"
            "-DRECREATION_EQUILIBRIUM_DIR=${equilibrium-src}"
            # rx looks for kinema beside its own source; in the store it is not
            # there, so hand it the path.
            "-DRX_KINEMA_DIR=${kinema-src}"
            "-DRECREATION_AUDIO_FFMPEG=ON"
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
            # /run/opengl-driver/lib is the NixOS driver dir; the Debian-style
            # paths cover other hosts. The nvidia libs land in the symlink dir so
            # the loader can resolve them by soname without the whole dir on the
            # path. libnvidia-ngx (the NGX core behind DLSS) is dlopened this way.
            # libcuda/libnvcuextend too: the driver's ray tracing stack dlopens
            # libcuda.so.1 (observed on GB10), and without it vkCreateDevice with
            # the acceleration-structure extensions fails INITIALIZATION_FAILED.
            for f in /usr/lib/*-linux-gnu/libnvidia*.so* /usr/lib/*-linux-gnu/libGLX_nvidia.so* \
                     /usr/lib/*-linux-gnu/libEGL_nvidia.so* \
                     /usr/lib/*-linux-gnu/libcuda.so* /usr/lib/*-linux-gnu/libnvcuextend.so* \
                     /run/opengl-driver/lib/libnvidia*.so* /run/opengl-driver/lib/libGLX_nvidia.so* \
                     /run/opengl-driver/lib/libEGL_nvidia.so* \
                     /run/opengl-driver/lib/libcuda.so* /run/opengl-driver/lib/libnvcuextend.so*; do
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
              directx-shader-compiler  # dxc, hlsl -> spirv
              shader-slang             # slangc, rx's .slang shaders
              glslang                  # FidelityFX shader permutations
              pkg-config               # libultragui finds freetype/harfbuzz
              freetype                 # libultragui text rasterization
              harfbuzz                 # libultragui text shaping
              glib                     # harfbuzz.pc requires glib-2.0
              fontconfig               # fc-match for the hud font
              sdl3
              wayland                  # libwayland-client, KDE HDR-toggle monitor
              ffmpeg                   # libav* for compressed audio (xWMA/Wwise/FUZ)
              openssl  # zetanet crypto backend
              dotnet-sdk_9             # .net core clr host + managed build
              vulkan-headers
              vulkan-loader
              vulkan-validation-layers
              vulkan-tools
              (nanocFor pkgs)         # nanobuf schema compiler, for nanobuf_regen
              (vkd3dFor pkgs)         # native D3D12-on-Vulkan, for the d3d12 rhi backend
              # mbedTLS (zetanet's crypto backend wherever there is no system
              # OpenSSL, i.e. every cross build) generates its PSA driver
              # wrappers with a python script that needs these two.
              (pkgs.python3.withPackages (ps: with ps; [ jsonschema jinja2 ]))
              vkrun
              swrun
            ];

            shellHook = ''
              export VK_LAYER_PATH=${pkgs.vulkan-validation-layers}/share/vulkan/explicit_layer.d
              export RECREATION_FETCHCONTENT_FLAGS="${builtins.concatStringsSep " " fetchContentFlags}"

              # .NET CLR host: the C++ side dlopens libhostfxr.so out of the
              # runtime tree under DOTNET_ROOT, and the managed build needs the
              # sdk on PATH (nixpkgs ships it non-relocatable, so point at it).
              export DOTNET_ROOT=${pkgs.dotnet-sdk_9}/share/dotnet
              export DOTNET_CLI_TELEMETRY_OPTOUT=1
              export DOTNET_NOLOGO=1

              if [ -z "''${DISPLAY:-}" ] && [ -S /tmp/.X11-unix/X0 ]; then
                export DISPLAY=:0
              fi
            '';
          };
        });
    };
}
