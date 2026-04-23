#!/bin/bash
set -e

curl -fsSL https://cli.github.com/packages/githubcli-archive-keyring.gpg \
    | dd of=/usr/share/keyrings/githubcli-archive-keyring.gpg
chmod go+r /usr/share/keyrings/githubcli-archive-keyring.gpg
echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/githubcli-archive-keyring.gpg] https://cli.github.com/packages stable main" \
    > /etc/apt/sources.list.d/github-cli.list
apt-get update
apt-get -y install --no-install-recommends gh

# NOTE: To persist authentication across devcontainer rebuilds, authenticate on
# the host using --insecure-storage so the token is written to ~/.config/gh/hosts.yml
# (the bind-mounted file) instead of the system keyring, which is inaccessible
# inside the container. See: https://github.com/cli/cli/discussions/7611#discussioncomment-8083850
#
#   gh auth login --insecure-storage
