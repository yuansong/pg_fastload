# Build container for the binary release assets.
#
# One image builds one (PostgreSQL major, RHEL major) pair.  It compiles the
# extension and the CLI against the distribution's PostgreSQL, packs them with
# rpmbuild, then installs the package it just produced and runs the regression
# suite against that installation -- so a release never carries a package whose
# own tests did not pass on the platform it was built for.
#
# The source is taken from GIT_REF (the release tag), not from the default
# branch, and the tarball fed to rpmbuild is the same git archive that is
# published next to the RPMs.

ARG RHEL_VERSION
FROM rockylinux/rockylinux:${RHEL_VERSION:-10}

ARG RHEL_VERSION
ARG PG_VERSION
ARG PG_FASTLOAD_VERSION
ARG REPO_URL
ARG GIT_REF=master

ENV PATH=/usr/pgsql-${PG_VERSION}/bin:$PATH
ENV PGDATA=/var/lib/pgsql/${PG_VERSION}/data


################################################################################
#
# Prerequisite
#
################################################################################

# Install packages for build
RUN sed -i -e '/^mirrorlist=/s/^/#/' -e '/^#baseurl=/s/^#//' /etc/yum.repos.d/*.repo && \
    dnf -y --setopt=retries=5 update && \
    dnf -y --setopt=retries=5 install \
        clang clang-devel gcc git krb5-devel libselinux-devel libzstd-devel lz4-devel make \
        openssl-devel pam-devel readline-devel rpmdevtools zlib-devel \
        numactl-devel

# CodeReady Builder carries the LLVM bits PostgreSQL's bitcode build needs.  It
# was called powertools before 8.10, and the old name is gone on current Rocky 8
# images, so enable whichever one this release actually has.
RUN (dnf -y install 'dnf-command(config-manager)' || true) && \
    (dnf config-manager --set-enabled crb || \
     dnf config-manager --set-enabled powertools || true)

# Install PostgreSQL
RUN dnf -y --setopt=retries=5 install \
        https://download.postgresql.org/pub/repos/yum/reporpms/EL-${RHEL_VERSION}-x86_64/pgdg-redhat-repo-latest.noarch.rpm
RUN if [ "${RHEL_VERSION}" = "8" ] || [ "${RHEL_VERSION}" = "9" ]; then \
        dnf -qy module disable postgresql; \
    fi
RUN dnf -y --setopt=retries=5 install \
        postgresql${PG_VERSION}-server \
        postgresql${PG_VERSION}-devel \
        postgresql${PG_VERSION}-llvmjit


################################################################################
#
# Build RPMs
#
################################################################################

# Build by postgres user
USER postgres
WORKDIR /var/lib/pgsql/

# Deploy the files required by the build
RUN rpmdev-setuptree
RUN git clone --depth 1 --branch "${GIT_REF}" "${REPO_URL}" src
RUN cp -a src/SPECS/pg_fastload-pg${PG_VERSION}.spec rpmbuild/SPECS/
RUN cd src && \
    git archive \
        --format=tar.gz \
        --prefix=pg_fastload-${PG_FASTLOAD_VERSION}/ \
        --output=../rpmbuild/SOURCES/pg_fastload-${PG_FASTLOAD_VERSION}.tar.gz \
        HEAD

# Build RPMs
RUN QA_RPATHS=0x0002 rpmbuild rpmbuild/SPECS/pg_fastload-pg${PG_VERSION}.spec \
        -bb --define="dist .pg${PG_VERSION}.rhel${RHEL_VERSION}"


################################################################################
#
# Run regression tests against the package, not against the build tree
#
################################################################################

USER root
RUN rpm -ivh /var/lib/pgsql/rpmbuild/RPMS/x86_64/*.rpm

USER postgres
# A build container gets a 64 MiB /dev/shm, which is where PostgreSQL puts both
# its main shared memory and the dynamic segments the parallel path allocates.
# The suite passes on a normal host but trips here under load, so give this
# step a tmpfs of its own; the server settings stay at their defaults.
RUN --mount=type=tmpfs,target=/dev/shm \
    mkdir -p /var/lib/pgsql/check && \
    tar -xzf /var/lib/pgsql/rpmbuild/SOURCES/pg_fastload-${PG_FASTLOAD_VERSION}.tar.gz \
        -C /var/lib/pgsql/check --strip-components=1 && \
    cd /var/lib/pgsql/check && \
    echo "cores: $(nproc)"; df -h /dev/shm | tail -n 1; \
    initdb --no-locale -E UTF8 && \
    pg_ctl -w start -l /var/lib/pgsql/check/server.log && \
    ( make USE_PGXS=1 installcheck > /tmp/check.log 2>&1; echo $? > /tmp/check.rc ); \
    rc=$(cat /tmp/check.rc); \
    if [ "$rc" -ne 0 ]; then \
        echo "=== installcheck failed (rc=$rc) ==="; \
        grep -nE '^(ok|not ok|# )' /tmp/check.log | tail -n 20; \
        echo '=== regression.diffs, first 120 lines ==='; \
        sed -n '1,120p' bin/regression.diffs 2>/dev/null || true; \
        echo '=== server.log, last 40 lines ==='; \
        tail -n 40 /var/lib/pgsql/check/server.log 2>/dev/null || true; \
    fi; \
    test "$rc" -eq 0
