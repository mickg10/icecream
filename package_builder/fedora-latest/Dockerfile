FROM fedora:latest

COPY build_rpm.sh /usr/local/bin/build_rpm.sh
RUN chmod +x /usr/local/bin/build_rpm.sh

COPY verify_rpm.sh /usr/local/bin/verify_rpm.sh
RUN chmod +x /usr/local/bin/verify_rpm.sh

WORKDIR /work
