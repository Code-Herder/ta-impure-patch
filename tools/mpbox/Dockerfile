# The box a multiplayer test runs in: one per test, with its own loopback.
#
# DirectPlay's name server owns UDP 47624 for whatever machine it runs on, so two games hosting
# at once collide however separate their prefixes are. A container with `--network none` has a
# loopback of its own, so the peers of one test reach each other over 127.0.0.1 and no other
# test's peers exist to them -- which is what lets several run at the same time.
#
# IT MUST MATCH THE HOST, because the instance prefixes are bind-mounted in and a wine of another
# version would update them in place: Ubuntu 24.04, wine 9.0 from the distribution, the same
# packages `dpkg -l | grep wine` lists outside.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN dpkg --add-architecture i386 && apt-get update && apt-get install -y --no-install-recommends \
        wine wine32:i386 wine64 libwine:i386 libwine fonts-wine \
        xvfb xdotool x11-utils x11-xserver-utils imagemagick \
        python3 python3-tk ca-certificates procps psmisc net-tools iproute2 \
    && rm -rf /var/lib/apt/lists/*

# The game runs as the owner's own uid, because the prefixes it writes are theirs on the host.
# The uid is passed at `docker run --user`, so no user is baked in here; HOME is given there too.
ENV WINEDEBUG=-all

# A shell is all the entry point has to be: every command is a `docker exec` of tacli.
CMD ["sleep", "infinity"]
