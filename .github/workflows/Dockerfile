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
    dnf update -y && \
    dnf install -y \
        clang gcc git krb5-devel libselinux-devel libzstd-devel lz4-devel make \
        openssl-devel pam-devel readline-devel rpmdevtools zlib-devel \
        numactl-devel

# Install PostgreSQL
RUN if [ "${RHEL_VERSION}" = "8" ]; then \
        dnf install -y --enablerepo=powertools perl-IPC-Run; \
    else \
        dnf install -y --enablerepo=crb perl-IPC-Run; \
    fi
RUN dnf install -y https://download.postgresql.org/pub/repos/yum/reporpms/EL-${RHEL_VERSION}-x86_64/pgdg-redhat-repo-latest.noarch.rpm
RUN if [ "${RHEL_VERSION}" = "8" ] || [ "${RHEL_VERSION}" = "9" ]; then \
        dnf -qy module disable postgresql; \
    fi
RUN dnf install -y \
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
RUN mkdir -p /var/lib/pgsql/check && \
    tar -xzf /var/lib/pgsql/rpmbuild/SOURCES/pg_fastload-${PG_FASTLOAD_VERSION}.tar.gz \
        -C /var/lib/pgsql/check --strip-components=1 && \
    cd /var/lib/pgsql/check && \
    initdb --no-locale -E UTF8 && \
    pg_ctl -w start -l /var/lib/pgsql/check/server.log && \
    make USE_PGXS=1 installcheck
