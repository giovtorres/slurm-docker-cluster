FROM rockylinux:9

LABEL org.opencontainers.image.source="https://github.com/sckyzo/slurm-docker-cluster" \
      org.opencontainers.image.title="Custom Slurm Docker Cluster" \
      org.opencontainers.image.description="Customized Slurm cluster image with RESTAPI, Elasticsearch, Grafana , based on Rocky Linux 9" \
      org.opencontainers.image.authors="Thomas Bourcey, modified by Jiadong Zhou" \
      org.opencontainers.image.maintainer="Jiadong Zhou" \
      org.opencontainers.image.licenses="MIT"

# Define build arguments
ARG USER=slurm
ARG PUID=990
ARG PGID=990 
ARG GOSU_VERSION=1.17
ARG SLURM_TAG=slurm-24-1-5
ARG RESTUID=995
ARG RESTGID=995 

# Install necessary packages and clean up cache
RUN set -ex \
    && dnf makecache \
    && dnf -y update \
    && dnf -y install dnf-plugins-core epel-release \
    && dnf config-manager --set-enabled crb --set-enabled epel \
    && dnf -y install \
       wget \
       bzip2 \
       perl \
       gcc \
       gcc-c++ \
       git \
       gnupg \
       make \
       munge \
       munge-devel \
       python3-devel \
       python3-pip \
       python3 \
       mariadb-server \
       mariadb-devel \
       psmisc \
       bash-completion \
       vim-enhanced \
       http-parser-devel \
       json-c-devel \
       s-nail libjwt libjwt-devel\
    && dnf clean all \
    && rm -rf /var/cache/dnf

# Install Python packages
RUN pip3 install Cython nose

# Download and install gosu
RUN set -ex \
    && wget -O /usr/local/bin/gosu "https://github.com/tianon/gosu/releases/download/$GOSU_VERSION/gosu-amd64" \
    && wget -O /usr/local/bin/gosu.asc "https://github.com/tianon/gosu/releases/download/$GOSU_VERSION/gosu-amd64.asc" \
    && export GNUPGHOME="$(mktemp -d)" \
    && gpg --batch --keyserver hkps://keys.openpgp.org --recv-keys B42F6819007F00F88E364FD4036A9C25BF357DD4 \
    && gpg --batch --verify /usr/local/bin/gosu.asc /usr/local/bin/gosu \
    && rm -rf "${GNUPGHOME}" /usr/local/bin/gosu.asc \
    && chmod +x /usr/local/bin/gosu \
    && gosu nobody true

COPY slurm /home/software/slurm
WORKDIR /home/software/slurm
# Clone and install Slurm
RUN set -x \
    && ./configure --enable-debug --prefix=/usr --sysconfdir=/etc/slurm \
        --with-mysql_config=/usr/bin  --libdir=/usr/lib64 --with-jwt --enable-slurmrestd --enable-\
    && make install \
    && install -D -m644 etc/cgroup.conf.example /etc/slurm/cgroup.conf.example \
    && install -D -m644 etc/slurm.conf.example /etc/slurm/slurm.conf.example \
    && install -D -m644 etc/slurmdbd.conf.example /etc/slurm/slurmdbd.conf.example \
    && install -D -m644 contribs/slurm_completion_help/slurm_completion.sh /etc/profile.d/slurm_completion.sh \
    && groupadd -r --gid=$PGID $USER \
    && useradd -r -g $USER --uid=$PUID $USER \

    && mkdir /etc/sysconfig/slurm \
        /var/spool/slurmd \
        /var/run/slurmd \
        /var/run/slurmdbd \
        /var/lib/slurmd \
        /var/log/slurm \
        /data \
    && touch /var/lib/slurmd/node_state \
        /var/lib/slurmd/front_end_state \
        /var/lib/slurmd/job_state \
        /var/lib/slurmd/resv_state \
        /var/lib/slurmd/trigger_state \
        /var/lib/slurmd/assoc_mgr_state \
        /var/lib/slurmd/assoc_usage \
        /var/lib/slurmd/qos_usage \
        /var/lib/slurmd/fed_mgr_state \
    && chown -R slurm:slurm /var/*/slurm* \
    && /sbin/create-munge-key

RUN groupadd -r -g $RESTGID restd \
 && useradd  -r -u $RESTUID -g restd -G munge -s /sbin/nologin slurmrestd

VOLUME /etc/slurm

# Copy configuration files
COPY conf_files/etc/slurm/slurm.conf /etc/slurm/slurm.conf
COPY conf_files/etc/slurm/slurmdbd.conf /etc/slurm/slurmdbd.conf
COPY conf_files/etc/slurm/cgroup.conf /etc/slurm/cgroup.conf

RUN set -x \
    && chown $USER:$USER /etc/slurm/slurmdbd.conf \
    && chmod 600 /etc/slurm/slurmdbd.conf

RUN head -c32 /dev/urandom | base64 > /etc/slurm/jwt_hmac.key \
    && chown slurm:restd /etc/slurm/jwt_hmac.key \
    && chmod 640 /etc/slurm/jwt_hmac.key \
    && chmod 644 /etc/slurm/slurm.conf

COPY conf_files/usr/local/bin/docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]

RUN dnf install -y \
        libcurl-devel \
        pam-devel \
        numactl-devel \
        openssl-devel\ 
        readline-devel\ 
        gtk2-devel\ 
        perl-ExtUtils-MakeMaker

WORKDIR /home/software/slurm
RUN ./configure \
    --prefix=/usr \
    --sysconfdir=/etc/slurm \
    --with-json \
  && cd src/plugins/jobcomp/elasticsearch \
  && make -j$(nproc) \
  && cp .libs/jobcomp_elasticsearch.so /usr/lib64/slurm/


CMD ["slurmdbd"]
