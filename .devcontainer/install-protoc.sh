#!/bin/bash
set -e

# Official protoc release matching the third_party/protobuf submodule pin.
# protoc emits an exact-match version guard against the vendored runtime, so
# this version must move in lockstep with that submodule's tag.
PROTOC_VERSION=36.0

curl -sSL -o /tmp/protoc.zip \
    "https://github.com/protocolbuffers/protobuf/releases/download/v${PROTOC_VERSION}/protoc-${PROTOC_VERSION}-linux-x86_64.zip"
unzip -q /tmp/protoc.zip -d /usr/local
rm /tmp/protoc.zip
chmod +x /usr/local/bin/protoc
