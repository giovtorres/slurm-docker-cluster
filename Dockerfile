# Multi-stage Dockerfile for Slurm runtime
# Stage 1: Build gosu from source with latest Go (avoids CVEs in pre-built binaries)
# Stage 2: Build RPMs using the builder image
# Stage 3: Install RPMs in a clean runtime image

ARG SLURM_VERSION
ARG GOSU_VERSION=1.19
ARG LMOD_VERSION=9.1.2
ARG SPACK_VERSION=v1.1.1
# BUILDER_BASE and RUNTIME_BASE overridden when GPU_ENABLE=true is set in .env
ARG BUILDER_BASE=rockylinux/rockylinux:9
ARG RUNTIME_BASE=rockylinux/rockylinux:9

# ============================================================================
# Stage 1: Build gosu from source
# (pre-built binaries use an old Go version that triggers CVEs)
# https://github.com/tianon/gosu/issues/136
# ============================================================================
FROM golang:1.26-bookworm AS gosu-builder

ARG GOSU_VERSION
ARG TARGETOS
ARG TARGETARCH

RUN set -ex \
    && git clone --branch ${GOSU_VERSION} --depth 1 \
       https://github.com/tianon/gosu.git /go/src/github.com/tianon/gosu \
    && cd /go/src/github.com/tianon/gosu \
    && go mod download \
    && CGO_ENABLED=0 GOOS=${TARGETOS} GOARCH=${TARGETARCH} \
       go build -v -trimpath -ldflags '-d -w' \
       -o /go/bin/gosu . \
    && chmod +x /go/bin/gosu

# ============================================================================
# Stage 2: Build RPMs
# ============================================================================
FROM ${BUILDER_BASE} AS builder

ARG SLURM_VERSION
ARG TARGETARCH
ARG GPU_ENABLE

# Enable CRB and EPEL repositories for development packages
# Install RPM build tools and dependencies
RUN set -ex \
    && dnf makecache \
    && dnf -y install dnf-plugins-core epel-release \
    && dnf config-manager --set-enabled crb \
    && dnf makecache \
    && dnf -y install --nobest --exclude='*.i686' \
       autoconf \
       automake \
       bzip2 \
       freeipmi-devel \
       dbus-devel \
       gcc \
       gcc-c++ \
       git \
       gtk2-devel \
       hdf5-devel \
       http-parser-devel \
       hwloc-devel \
       json-c-devel \
       libcurl-devel \
       libyaml-devel \
       lua-devel \
       lz4-devel \
       make \
       man2html \
       mariadb-devel \
       munge \
       munge-devel \
       ncurses-devel \
       numactl-devel \
       openssl-devel \
       pam-devel \
       perl \
       python3.12 \
       python3.12-devel \
       readline-devel \
       rpm-build \
       rpmdevtools \
       rrdtool-devel \
       wget \
       libjwt \
       libjwt-devel \
    && dnf clean all \
    && rm -rf /var/cache/dnf

# Setup RPM build environment
RUN rpmdev-setuptree

# Copy RPM macros
COPY rpmbuild/slurm.rpmmacros /root/.rpmmacros

# When GPU_ENABLE=true, the builder base image (nvidia/cuda:*-devel-*) provides
# NVML headers. Symlink them to standard paths and enable --with-nvml for rpmbuild.
RUN if [ "${GPU_ENABLE}" = "true" ]; then \
        set -ex && \
        CUDA_TARGET=$(case "${TARGETARCH}" in amd64) echo "x86_64-linux" ;; arm64) echo "sbsa-linux" ;; esac) && \
        ln -sf /usr/local/cuda/include/nvml.h /usr/include/nvml.h && \
        ln -sf /usr/local/cuda/targets/${CUDA_TARGET}/lib/stubs/libnvidia-ml.so /usr/lib64/libnvidia-ml.so && \
        echo "%_with_nvml --with-nvml=/usr" >> /root/.rpmmacros; \
    fi

# Download official Slurm release tarball and build RPMs with slurmrestd enabled
# Architecture mapping: Docker TARGETARCH (amd64, arm64) -> RPM arch (x86_64, aarch64)
RUN set -ex \
    && RPM_ARCH=$(case "${TARGETARCH}" in \
         amd64) echo "x86_64" ;; \
         arm64) echo "aarch64" ;; \
         *) echo "Unsupported architecture: ${TARGETARCH}" && exit 1 ;; \
       esac) \
    && echo "Building Slurm RPMs for architecture: ${RPM_ARCH}" \
    && wget -O /root/rpmbuild/SOURCES/slurm-${SLURM_VERSION}.tar.bz2 \
       https://download.schedmd.com/slurm/slurm-${SLURM_VERSION}.tar.bz2 \
    && cd /root/rpmbuild/SOURCES \
    && rpmbuild -ta slurm-${SLURM_VERSION}.tar.bz2 \
    && ls -lh /root/rpmbuild/RPMS/${RPM_ARCH}/

# ============================================================================
# Stage 3: Build Lmod from source
# (hardcoded Rocky Linux 9 base — GPU CUDA images are not needed to build Lmod)
# ============================================================================
FROM rockylinux/rockylinux:9 AS lmod-builder

ARG LMOD_VERSION

RUN set -ex \
    && dnf -y install dnf-plugins-core epel-release \
    && dnf config-manager --set-enabled crb \
    && dnf -y install \
       bc \
       gcc \
       lua \
       lua-devel \
       lua-posix \
       lua-filesystem \
       tcl \
       tcl-devel \
       procps-ng \
       python3 \
       wget \
    && dnf clean all \
    && rm -rf /var/cache/dnf

RUN set -ex \
    && wget -O /tmp/Lmod-${LMOD_VERSION}.tar.gz \
       https://github.com/TACC/Lmod/archive/refs/tags/${LMOD_VERSION}.tar.gz \
    && tar -xzf /tmp/Lmod-${LMOD_VERSION}.tar.gz -C /tmp \
    && cd /tmp/Lmod-${LMOD_VERSION} \
    && ./configure --prefix=/usr/local \
    && make install

# ============================================================================
# Stage 4: Clone Spack from source
# (hardcoded Rocky Linux 9 — no GPU overhead needed for this stage)
# ============================================================================
FROM rockylinux/rockylinux:9 AS spack-builder

ARG SPACK_VERSION

RUN dnf -y install git gcc && dnf clean all

RUN git clone --depth=1 --branch "${SPACK_VERSION}" \
        https://github.com/spack/spack.git /usr/local/spack

# Write site-level Spack config:
# - modules.yaml: generate Lmod modulefiles using the system GCC as the core compiler
# - config.yaml:  store installed packages and generated modules under /opt/spack (the named volume)
# - packages.yaml: pin target to the base ISA (x86_64 or aarch64) so the module
#                  path is predictable and matches the MODULEPATH baked into lmod.sh
RUN set -ex \
    && GCC_VER=$(gcc --version | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1) \
    && ARCH=$(uname -m) \
    && mkdir -p /usr/local/spack/etc/spack \
    && printf 'modules:\n  default:\n    enable:\n      - lmod\n    roots:\n      lmod: /opt/spack/modules\n    lmod:\n      core_compilers:\n        - gcc@%s\n' \
         "${GCC_VER}" > /usr/local/spack/etc/spack/modules.yaml \
    && printf 'config:\n  install_tree:\n    root: /opt/spack\n' \
         > /usr/local/spack/etc/spack/config.yaml \
    && printf 'packages:\n  all:\n    require:\n      - "target=%s"\n' \
         "${ARCH}" > /usr/local/spack/etc/spack/packages.yaml

# ============================================================================
# Stage 5: Runtime image
# ============================================================================
FROM ${RUNTIME_BASE}

LABEL org.opencontainers.image.source="https://github.com/giovtorres/slurm-docker-cluster" \
      org.opencontainers.image.title="slurm-docker-cluster" \
      org.opencontainers.image.description="Slurm Docker cluster on Rocky Linux 9" \
      maintainer="Giovanni Torres"

ARG SLURM_VERSION
ARG TARGETARCH
ARG GPU_ENABLE

# Enable CRB and EPEL repositories, then install runtime dependencies
RUN set -ex \
    && dnf -y update \
    && dnf -y install dnf-plugins-core epel-release \
    && dnf config-manager --set-enabled crb \
    && dnf makecache \
    && dnf -y install \
       apptainer \
       bash-completion \
       bzip2 \
       bzip2-devel \
       file \
       gcc \
       gcc-c++ \
       gcc-gfortran \
       gettext \
       git \
       hdf5 \
       http-parser \
       hwloc \
       json-c \
       jq \
       libaec \
       libyaml \
       lua \
       lua-posix \
       lua-filesystem \
       lz4 \
       make \
       mariadb \
       munge \
       numactl \
       openssh-server \
       patch \
       perl \
       procps-ng \
       psmisc \
       python3.12 \
       readline \
       tcl \
       vim-enhanced \
       wget \
       xz \
       libjwt \
    && dnf clean all \
    && rm -rf /var/cache/dnf \
    && alternatives --install /usr/bin/python3 python3 /usr/bin/python3.12 1 \
    && alternatives --set python3 /usr/bin/python3.12 \
    && ssh-keygen -A \
    && sed -i 's/^#\?PermitRootLogin.*/PermitRootLogin prohibit-password/' /etc/ssh/sshd_config \
    && sed -i 's/^#\?PasswordAuthentication.*/PasswordAuthentication no/' /etc/ssh/sshd_config

# Install gosu
COPY --from=gosu-builder /go/bin/gosu /usr/local/bin/gosu
RUN gosu --version && gosu nobody true

# Install Lmod
COPY --from=lmod-builder /usr/local/lmod /usr/local/lmod

# Install Spack
COPY --from=spack-builder /usr/local/spack /usr/local/spack

# Configure Lmod system-wide and source Spack's shell integration.
# MODULEPATH uses the base ISA arch (x86_64/aarch64 from uname -m) which matches
# the target forced in packages.yaml, avoiding microarch mismatches (e.g. zen2).
RUN ARCH=$(uname -m) \
    && printf '%s\n' \
      'source /usr/local/lmod/lmod/init/bash' \
      'export SPACK_ROOT=/usr/local/spack' \
      "export MODULEPATH=\"/opt/spack/modules/linux-rocky9-${ARCH}/Core:/opt/modulefiles\"" \
      'source /usr/local/spack/share/spack/setup-env.sh' \
      > /etc/profile.d/lmod.sh \
    && chmod 644 /etc/profile.d/lmod.sh \
    && mkdir -p /opt/modulefiles /opt/spack /opt/spack/modules

COPY --from=builder /root/rpmbuild/RPMS/*/*.rpm /tmp/rpms/

# Install Slurm RPMs
RUN set -ex \
    && dnf -y install /tmp/rpms/slurm-[0-9]*.rpm \
       /tmp/rpms/slurm-perlapi-*.rpm \
       /tmp/rpms/slurm-slurmctld-*.rpm \
       /tmp/rpms/slurm-slurmd-*.rpm \
       /tmp/rpms/slurm-slurmdbd-*.rpm \
       /tmp/rpms/slurm-slurmrestd-*.rpm \
       /tmp/rpms/slurm-contribs-*.rpm \
       /tmp/rpms/slurm-devel-*.rpm \
    && mkdir -p /var/cache/slurm-rpms \
    && cp /tmp/rpms/slurm-[0-9]*.rpm /tmp/rpms/slurm-contribs-*.rpm /tmp/rpms/slurm-devel-*.rpm /var/cache/slurm-rpms/ \
    && rm -rf /tmp/rpms \
    && dnf clean all

# Create users, generate munge key, and set up directories
RUN set -x \
    && groupadd -r --gid=990 slurm \
    && useradd -r -g slurm --uid=990 slurm \
    && groupadd -r --gid=991 slurmrest \
    && useradd -r -g slurmrest --uid=991 slurmrest \
    && chmod 0755 /etc \
    && /sbin/create-munge-key \
    && mkdir -m 0755 -p \
        /var/run/slurm \
        /var/spool/slurm \
        /var/lib/slurm \
        /var/log/slurm \
        /etc/slurm \
    && chown slurm:slurm \
        /var/run/slurm \
        /var/spool/slurm \
        /var/lib/slurm \
        /var/log/slurm \
        /etc/slurm \
    && useradd -m -u 1001 ood

# Copy Slurm configuration files
# Version-specific configs: Extract major.minor from SLURM_VERSION (e.g., "24.11" from "24.11.6")
COPY config/ /tmp/slurm-config/
RUN set -ex \
    && MAJOR_MINOR=$(echo ${SLURM_VERSION} | cut -d. -f1,2) \
    && echo "Detected Slurm version: ${MAJOR_MINOR}" \
    && if [ -f "/tmp/slurm-config/${MAJOR_MINOR}/slurm.conf" ]; then \
         echo "Using version-specific config for ${MAJOR_MINOR}"; \
         cp /tmp/slurm-config/${MAJOR_MINOR}/slurm.conf /etc/slurm/slurm.conf; \
       else \
         echo "No version-specific config found for ${MAJOR_MINOR}, using latest (25.11)"; \
         cp /tmp/slurm-config/25.11/slurm.conf /etc/slurm/slurm.conf; \
       fi \
    && cp /tmp/slurm-config/common/slurmdbd.conf /etc/slurm/slurmdbd.conf \
    && cp /tmp/slurm-config/common/job_submit.lua /etc/slurm/job_submit.lua \
    && if [ -f "/tmp/slurm-config/${MAJOR_MINOR}/cgroup.conf" ]; then \
         echo "Using version-specific cgroup.conf for ${MAJOR_MINOR}"; \
         cp /tmp/slurm-config/${MAJOR_MINOR}/cgroup.conf /etc/slurm/cgroup.conf; \
       else \
         echo "Using common cgroup.conf"; \
         cp /tmp/slurm-config/common/cgroup.conf /etc/slurm/cgroup.conf; \
       fi \
    && if [ "$GPU_ENABLE" = "true" ]; then \
         echo "GPU support enabled, installing gres.conf"; \
         cp /tmp/slurm-config/common/gres.conf /etc/slurm/gres.conf; \
         chown slurm:slurm /etc/slurm/gres.conf; \
         chmod 644 /etc/slurm/gres.conf; \
       else \
         echo "GPU support disabled, skipping gres.conf"; \
       fi \
    && chown slurm:slurm /etc/slurm/slurm.conf /etc/slurm/cgroup.conf /etc/slurm/slurmdbd.conf /etc/slurm/job_submit.lua \
    && chmod 644 /etc/slurm/slurm.conf /etc/slurm/cgroup.conf \
    && chmod 600 /etc/slurm/slurmdbd.conf \
    && rm -rf /tmp/slurm-config

COPY --chown=slurm:slurm --chmod=0600 examples /root/examples

COPY docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
RUN chmod +x /usr/local/bin/docker-entrypoint.sh

ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]

CMD ["slurmdbd"]
