#!/bin/bash

# NOTE: pushes to Infactum's own Docker Hub namespace - this fork has no
# push access there, and the Dockerfile.* recipes this builds predate the
# tgcalls/WebRTC migration and TDLib master bump (see
# .github/workflows-disabled/main.yml.needs-rewrite for why CI is currently
# disabled). Needs its own DOCKER_IMAGE namespace and an updated recipe
# before this is usable again.

set -e

DOCKER_IMAGE=infactum/tg2sip-builder

for DOCKER_TAG in bionic centos7
do
    docker build . -f Dockerfile."$DOCKER_TAG" -t "$DOCKER_IMAGE:$DOCKER_TAG"
    docker push "$DOCKER_IMAGE:$DOCKER_TAG"
done
