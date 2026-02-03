# Multi-stage Dockerfile for Slurm runtime
# Stage 1: Build RPMs using the builder image
# Stage 2: Install RPMs in a clean runtime image

ARG SLURM_VERSION

# ============================================================================
# Stage 1: Build RPMs
# ============================================================================
FROM rockylinux/rockylinux:9 AS builder

ARG SLURM_VERSION
ARG TARGETARCH

# Enable CRB and EPEL repositories for development packages
# Install RPM build tools and dependencies
RUN set -ex \
    && dnf makecache \
    && dnf -y update \
    && dnf -y install dnf-plugins-core epel-release \
    && dnf config-manager --set-enabled crb \
    && dnf makecache \
    && dnf -y install \
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
       python3 \
       python3-devel \
       readline-devel \
       rpm-build \
       rpmdevtools \
       rrdtool-devel \
       wget \
    && dnf clean all \
    && rm -rf /var/cache/dnf

# Setup RPM build environment
RUN rpmdev-setuptree

# Copy RPM macros
COPY rpmbuild/slurm.rpmmacros /root/.rpmmacros

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
# Stage 2: Runtime image
# ============================================================================
FROM rockylinux/rockylinux:9

LABEL org.opencontainers.image.source="https://github.com/giovtorres/slurm-docker-cluster" \
      org.opencontainers.image.title="slurm-docker-cluster" \
      org.opencontainers.image.description="Slurm Docker cluster on Rocky Linux 9" \
      maintainer="Giovanni Torres"

ARG SLURM_VERSION
ARG TARGETARCH

# Enable CRB and EPEL repositories for runtime dependencies
RUN set -ex \
    && dnf makecache \
    && dnf -y update \
    && dnf -y install dnf-plugins-core epel-release \
    && dnf config-manager --set-enabled crb \
    && dnf makecache

# Install runtime dependencies only
RUN set -ex \
    && dnf -y install \
       bash-completion \
       bzip2 \
       gettext \
       hdf5 \
       http-parser \
       hwloc \
       json-c \
       jq \
       libaec \
       libyaml \
       lua \
       lz4 \
       mariadb \
       munge \
       numactl \
       perl \
       procps-ng \
       psmisc \
       python3 \
       readline \
       vim-enhanced \
       wget \
    && dnf clean all \
    && rm -rf /var/cache/dnf

# Install gosu for privilege dropping
ARG GOSU_VERSION=1.19

RUN set -ex \
    && ARCH="$(uname -m)" \
    && case "$ARCH" in \
        x86_64) GOSU_ARCH='amd64'; GOSU_SHA256='bbc4136d03ab138b1ad66fa4fc051bafc6cc7ffae632b069a53657279a450de3' ;; \
        aarch64) GOSU_ARCH='arm64'; GOSU_SHA256='c3805a85d17f4454c23d7059bcb97e1ec1af272b90126e79ed002342de08389b' ;; \
        armv7l) GOSU_ARCH='armhf'; GOSU_SHA256='e5866286277ff2a2159fb9196fea13e0a59d3f1091ea46ddb985160b94b6841b' ;; \
        *) echo "Unsupported architecture: $ARCH" && exit 1 ;; \
    esac \
    && wget -O /usr/local/bin/gosu "https://github.com/tianon/gosu/releases/download/$GOSU_VERSION/gosu-$GOSU_ARCH" \
    && echo "$GOSU_SHA256  /usr/local/bin/gosu" | sha256sum -c - \
    && chmod +x /usr/local/bin/gosu \
    && gosu nobody true

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
    && rm -rf /tmp/rpms \
    && dnf clean all

# Install Singularity
RUN set -ex \
    && dnf -y install \
       apptainer \
    && dnf clean all \
    && rm -rf /var/cache/dnf

# Create slurm user and group
RUN set -x \
    && groupadd -r --gid=990 slurm \
    && useradd -r -g slurm --uid=990 slurm \
    && groupadd -r --gid=991 slurmrest \
    && useradd -r -g slurmrest --uid=991 slurmrest

# Fix /etc permissions and create munge key
RUN set -x \
    && chmod 0755 /etc \
    && /sbin/create-munge-key

# Create slurm dirs with correct ownership
RUN set -x \
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
        /etc/slurm

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
         echo "No version-specific config found for ${MAJOR_MINOR}, using latest (25.05)"; \
         cp /tmp/slurm-config/25.05/slurm.conf /etc/slurm/slurm.conf; \
       fi \
    && cp /tmp/slurm-config/common/slurmdbd.conf /etc/slurm/slurmdbd.conf \
    && if [ -f "/tmp/slurm-config/${MAJOR_MINOR}/cgroup.conf" ]; then \
         echo "Using version-specific cgroup.conf for ${MAJOR_MINOR}"; \
         cp /tmp/slurm-config/${MAJOR_MINOR}/cgroup.conf /etc/slurm/cgroup.conf; \
       else \
         echo "Using common cgroup.conf"; \
         cp /tmp/slurm-config/common/cgroup.conf /etc/slurm/cgroup.conf; \
       fi \
    && chown slurm:slurm /etc/slurm/slurm.conf /etc/slurm/cgroup.conf /etc/slurm/slurmdbd.conf \
    && chmod 644 /etc/slurm/slurm.conf /etc/slurm/cgroup.conf \
    && chmod 600 /etc/slurm/slurmdbd.conf \
    && rm -rf /tmp/slurm-config
COPY --chown=slurm:slurm --chmod=0600 examples /root/examples

COPY docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
RUN chmod +x /usr/local/bin/docker-entrypoint.sh

ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]

CMD ["slurmdbd"]
