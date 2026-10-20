# syntax=docker/dockerfile:1
# hadolint ignore=DL3007
FROM rust:latest AS rust
RUN cargo install --locked --git https://github.com/pavelxdd/cstyle --tag 1.1.4 --root /usr/local cstyle

FROM debian:sid-slim AS debian
WORKDIR /tmp

ARG TARGETARCH

ARG TZ=UTC
ARG LANG=C.UTF-8
ARG CC=/usr/bin/gcc
ARG CXX=/usr/bin/g++
ARG CPP=/usr/bin/cpp
ARG AR=/usr/bin/gcc-ar
ARG NM=/usr/bin/gcc-nm
ARG RANLIB=/usr/bin/gcc-ranlib
ARG OBJDUMP=/usr/bin/gcc-objdump
ARG OBJCOPY=/usr/bin/objcopy
ARG READELF=/usr/bin/readelf
ARG ADDR2LINE=/usr/bin/addr2line
ARG STRIP=/usr/bin/strip
ARG CFLAGS="-O2 -ftree-vectorize -pipe -g0 -fno-plt -fno-lto"
ARG CXXFLAGS="-O2 -ftree-vectorize -pipe -g0 -fno-plt -fno-lto -Wno-cpp"
ARG LDFLAGS="-fuse-ld=bfd -Wl,-O1,--strip-all,--sort-common,--as-needed,-z,relro,-z,now"

ENV DEBIAN_FRONTEND=noninteractive \
    APT_KEY_DONT_WARN_ON_DANGEROUS_USAGE=1 \
    GCC_VERSION=16 \
    LLVM_VERSION=23 \
    LANGUAGE=${LANG} \
    LANG=${LANG} \
    LC_ALL=${LANG} \
    TZ=${TZ}

RUN --mount=type=bind,target=/app \
    --mount=type=tmpfs,target=/tmp \
    --mount=type=tmpfs,target=/var/lib/apt \
    --mount=type=cache,target=/var/cache/apt,sharing=locked,id=debian-sid \
    \
    ln -snf "../usr/share/zoneinfo/${TZ}" /etc/localtime \
    && echo "${TZ}" > /etc/timezone \
    \
    && rm -f \
        /etc/apt/apt.conf.d/docker-clean \
        /etc/apt/sources.list.d/* \
        /etc/apt/sources.list \
        /etc/apt/preferences \
    \
    && install -m644 /app/docker/debian.sources -t /etc/apt/sources.list.d \
    && install -m644 /app/docker/apt.conf /etc/apt/apt.conf.d/99local \
    && install -m644 /app/docker/dpkg.cfg /etc/dpkg/dpkg.cfg.d/99local \
    \
    && apt-get update \
    && apt-get full-upgrade -yq --auto-remove --purge \
    && apt-get install -yq --no-install-recommends \
        ca-certificates gpg curl \
    \
    && sed -i 's/http:/https:/g' /etc/apt/sources.list.d/debian.sources \
    \
    && apt-get update \
    && apt-get full-upgrade -yq --auto-remove --purge \
    && apt-get install -yq --no-install-recommends \
        build-essential \
    \
    && apt-get install -yq --no-install-recommends \
        "gcc-${GCC_VERSION}" \
        "g++-${GCC_VERSION}" \
        "libgcc-${GCC_VERSION}-dev" \
    \
    && apt-get install -yq --no-install-recommends \
        "clang-${LLVM_VERSION}" \
        "clangd-${LLVM_VERSION}" \
        "clang-tidy-${LLVM_VERSION}" \
        "libclang-rt-${LLVM_VERSION}-dev" \
        "llvm-${LLVM_VERSION}-linker-tools" \
        "llvm-${LLVM_VERSION}-dev" \
        "lldb-${LLVM_VERSION}" \
        "lld-${LLVM_VERSION}" \
    \
    && apt-get install -yq --no-install-recommends \
        sudo \
        bash \
        bash-completion \
        nano \
        tmux \
        less \
        gnupg \
        openssh-client \
        sshpass \
        gdb \
        gdbserver \
        make \
        libtool \
        automake \
        autoconf \
        pkg-config \
        just \
        cmake \
        meson \
        ninja-build \
        linux-libc-dev \
        libcmocka-dev \
        libev-dev \
        libevent-dev \
        libuv1-dev \
        gcovr \
        valgrind \
        pre-commit \
        shellcheck \
        shfmt \
        yamllint \
        rsync \
        git \
        wget \
        file \
        tree \
        strace \
        python3 \
        python3-pip \
        python3-numpy \
        python3-matplotlib \
        zsh \
        zsh-autosuggestions \
        zsh-syntax-highlighting \
        starship \
    \
    && install -m755 -t /usr/local/bin /app/docker/update-alternatives-gcc \
    && update-alternatives-gcc "${GCC_VERSION}" 60 2> /dev/null \
    \
    && install -m755 -t /usr/local/bin /app/docker/update-alternatives-clang \
    && update-alternatives-clang "${LLVM_VERSION}" 60 2> /dev/null \
    \
    && rm -rf /var/lib/apt/lists/*

FROM debian as ci

# hadolint
RUN --mount=type=tmpfs,target=/tmp version=2.14.0 arch="" \
    && case "${TARGETARCH}" in \
        amd64) arch="x86_64" ;; \
            *) arch="${TARGETARCH}" ;; esac \
    && curl -fsSL "https://github.com/hadolint/hadolint/releases/download/v${version}/hadolint-linux-${arch}" \
             -o /usr/local/bin/hadolint \
    && chmod +x /usr/local/bin/hadolint

# yamlfmt
RUN --mount=type=tmpfs,target=/tmp version=0.21.0 arch="" \
    && case "${TARGETARCH}" in \
        amd64) arch="x86_64" ;; \
            *) arch="${TARGETARCH}" ;; esac \
    && name="yamlfmt_${version}_Linux_${arch}" \
    && curl -fsSL "https://github.com/google/yamlfmt/releases/download/v${version}/${name}.tar.gz" | tar xz \
    && install -s --strip-program="${STRIP}" -m755 -t /usr/local/bin yamlfmt

COPY --from=rust /usr/local/bin/cstyle /usr/local/bin/cstyle

ARG USERNAME="devcontainer"
ARG USER_UID="1000"
ARG USER_GID="${USER_UID}"

RUN --mount=type=tmpfs,target=/tmp user="${USERNAME}" uid="${USER_UID}" gid="${USER_GID}" \
    \
    && echo "ALL ALL=(ALL:ALL) NOPASSWD: ALL" > /etc/sudoers.d/nopasswd \
    && chmod 0440 /etc/sudoers.d/nopasswd \
    \
    && groupadd -o -g "${gid}" "${user}" \
    && useradd -o -u "${uid}" -g "${gid}" -G users,sudo -l -m "${user}" \
    \
    && mkdir -p "/home/${user}/.cache" \
    && mkdir -p "/home/${user}/.config" \
    && mkdir -p "/home/${user}/.local/bin" \
    && mkdir -p "/home/${user}/.local/lib" \
    && mkdir -p "/home/${user}/.local/share" \
    && mkdir -p "/home/${user}/.local/state" \
    && mkdir -p "/home/${user}/.vscode-server" \
    \
    && mkdir -p "/home/${user}/.ssh" \
    && chmod 700 "/home/${user}/.ssh" \
    \
    && mkdir -p "/home/${user}/.gnupg" \
    && chmod 700 "/home/${user}/.gnupg" \
    \
    && printf "[safe]\n\tdirectory = *\n" > "/home/${user}/.gitconfig" \
    \
    && chown -R "${uid}:${gid}" "/home/${user}" \
    \
    && mkdir -p "/app" \
    && chown "${uid}:${gid}" "/app"
