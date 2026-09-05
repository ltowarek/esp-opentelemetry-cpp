FROM espressif/idf:release-v6.0

RUN apt-get update && export DEBIAN_FRONTEND=noninteractive \
    && apt-get -y install --no-install-recommends \
    ccache \
    curl \
    locales \
    unzip

COPY install-gh.sh /tmp/install-gh.sh
RUN bash /tmp/install-gh.sh && rm /tmp/install-gh.sh

# Ubuntu's protobuf-compiler can't generate code for the vendored protobuf
# runtime pinned in third_party/protobuf (exact-match version guard); install
# the matching official release instead. Needed for the examples/ builds
# (WITH_OTLP_HTTP=ON) now that protoc comes from PATH instead of a host subbuild.
COPY install-protoc.sh /tmp/install-protoc.sh
RUN bash /tmp/install-protoc.sh && rm /tmp/install-protoc.sh

# WA for VSCode issue https://github.com/microsoft/vscode/issues/189924
RUN echo "en_US.UTF-8 UTF-8" >> /etc/locale.gen && locale-gen

ARG USERNAME=vscode
ARG USER_UID=1050
ARG USER_GID=$USER_UID

RUN groupadd --gid $USER_GID $USERNAME \
    && useradd --uid $USER_UID --gid $USER_GID --create-home --shell /bin/bash $USERNAME

ENV IDF_CCACHE_ENABLE=1
ENV CCACHE_DIR=/home/${USERNAME}/.ccache

RUN mkdir -p /home/$USERNAME/.claude && \
    chown -R $USERNAME:$USERNAME /home/$USERNAME/.claude

RUN mkdir -p /home/$USERNAME/.ccache && \
    chown -R $USERNAME:$USERNAME /home/$USERNAME/.ccache

USER $USERNAME

RUN git config --global --add safe.directory /opt/esp/idf/components/openthread/openthread
RUN echo "source $IDF_PATH/export.sh > /dev/null 2>&1" >> $HOME/.bashrc
RUN echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc

COPY --chown=$USERNAME:$USERNAME install-claude.sh /tmp/install-claude.sh
RUN bash /tmp/install-claude.sh && rm /tmp/install-claude.sh
