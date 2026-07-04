# Throwaway Asterisk PBX for pjsocky's live call tests
# (tests/protocol/test_live_call.py) - see run_live_tests.py, which
# builds and runs this. Plain distro Asterisk, no third-party image:
# the configuration in etc/ is bind-mounted over /etc/asterisk at run
# time, so config changes don't need an image rebuild.
FROM ubuntu:24.04

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        asterisk \
    && rm -rf /var/lib/apt/lists/*

# Foreground, console log only; config comes from the bind mount.
CMD ["asterisk", "-f", "-vvv"]
