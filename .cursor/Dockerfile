# System packages for Cloud Agents working on dpoler/FlightLevel314.
# Cursor checks out the repo into the workspace; do not COPY project sources here.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    cmake \
    curl \
    g++ \
    git \
    libcurl4-openssl-dev \
    libsdl2-dev \
    libstdc++-14-dev \
    pkg-config \
    python3 \
    python3-pip \
    python3-venv \
    && rm -rf /var/lib/apt/lists/*
