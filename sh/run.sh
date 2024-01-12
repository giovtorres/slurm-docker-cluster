#!/usr/bin/env bash
CWD="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)" && cd $CWD/..
set -ex

(
    yum makecache
    yum -y update
    yum -y install dnf-plugins-core
    yum config-manager --set-enabled crb
    dnf install -y \
        https://dl.fedoraproject.org/pub/epel/epel-release-latest-9.noarch.rpm \
        https://dl.fedoraproject.org/pub/epel/epel-next-release-latest-9.noarch.rpm
    dnf install -y git make cmake ccache tar gzip unzip \
        zlib-devel environment-modules wget m4 llvm \
        openmpi openmpi-devel hdf5 hdf5-devel perf \
        hdf5-openmpi-devel libasan libubsan \
        which g++ clang ninja-build pmix-devel mlocate
    yum -y install \
        wget \
        bzip2 \
        perl \
        gcc \
        gcc-c++ git \
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
        json-c-devel

    yum clean all && rm -rf /var/cache/yum /var/cache/dnf

    chmod +x sh/dnf_build_python3.11.sh && ./sh/dnf_build_python3.11.sh
    export PATH="/opt/py/py/bin:$PATH"
    which python3
    python3 -m pip install Cython nose

    wget -O /usr/local/bin/gosu "https://github.com/tianon/gosu/releases/download/$GOSU_VERSION/gosu-amd64"
    wget -O /usr/local/bin/gosu.asc "https://github.com/tianon/gosu/releases/download/$GOSU_VERSION/gosu-amd64.asc"
    export GNUPGHOME="$(mktemp -d)"
    gpg --batch --keyserver hkps://keys.openpgp.org --recv-keys B42F6819007F00F88E364FD4036A9C25BF357DD4
    gpg --batch --verify /usr/local/bin/gosu.asc /usr/local/bin/gosu
    rm -rf "${GNUPGHOME}" /usr/local/bin/gosu.asc
    chmod +x /usr/local/bin/gosu
    gosu nobody true

    git clone -b ${SLURM_TAG} --single-branch --depth=1 https://github.com/SchedMD/slurm.git
    pushd slurm
    ./configure --enable-debug --prefix=/usr --sysconfdir=/etc/slurm \
        --with-mysql_config=/usr/bin --libdir=/usr/lib64 --with-pmix
    make install
    install -D -m644 etc/cgroup.conf.example /etc/slurm/cgroup.conf.example
    install -D -m644 etc/slurm.conf.example /etc/slurm/slurm.conf.example
    install -D -m644 etc/slurmdbd.conf.example /etc/slurm/slurmdbd.conf.example
    install -D -m644 contribs/slurm_completion_help/slurm_completion.sh /etc/profile.d/slurm_completion.sh
    popd
    rm -rf slurm
    groupadd -r --gid=888 slurm
    useradd -r -g slurm --uid=888 slurm
    mkdir /etc/sysconfig/slurm \
        /var/spool/slurmd \
        /var/run/slurmd \
        /var/run/slurmdbd \
        /var/lib/slurmd \
        /var/log/slurm \
        /data
    touch /var/lib/slurmd/node_state \
        /var/lib/slurmd/front_end_state \
        /var/lib/slurmd/job_state \
        /var/lib/slurmd/resv_state \
        /var/lib/slurmd/trigger_state \
        /var/lib/slurmd/assoc_mgr_state \
        /var/lib/slurmd/assoc_usage \
        /var/lib/slurmd/qos_usage \
        /var/lib/slurmd/fed_mgr_state

    chown -R slurm:slurm /var/*/slurm*

    /sbin/create-munge-key && chmod 600 /etc/munge/munge.key && ls -l /etc/munge/munge.key

    chmod +x sh/dnf_build_ompi.sh && ./sh/dnf_build_ompi.sh

    yum clean all && rm -rf /var/cache/yum /var/cache/dnf

    # needs to be after OMPI build or OMPI build fails for sphinx reasons
    cd /root
    wget https://raw.githubusercontent.com/PHAREHUB/PHARE/master/requirements.txt
    which python3
    python3 -m pip install -r requirements.txt -U
    rm requirements.txt

) 1> >(tee $CWD/.run.sh.out) 2> >(tee $CWD/.run.sh.err >&2)
