DOCKER := $(shell { command -v podman || command -v docker; })
TIMESTAMP := $(shell date -u +"%Y%m%d%H%M")
COMMIT := $(shell git rev-parse --short HEAD 2>/dev/null)
ifeq ($(shell uname),Darwin)
SELINUX1 :=
SELINUX2 :=
else
SELINUX1 := :z
SELINUX2 := ,z
endif

# Only ask for a TTY when there is one (lets scripts/CI call make)
DOCKER_TTY := $(shell [ -t 0 ] && echo -it)

# RADIO=1 (default): the halves talk over the raw radio (zmk-modules/split-radio)
# with this pre-shared key, made once, never committed (.gitignore). Both
# halves must be built with the same key. RADIO=0: Bluetooth split (*-ble*).
RADIO ?= 1
SPLIT_RADIO_KEY := $(shell [ -s .split-radio-key ] || head -c16 /dev/urandom | od -An -tx1 | tr -d ' \n' > .split-radio-key; cat .split-radio-key)

.PHONY: all left test clean_firmware clean_image clean

all:
	$(shell bin/get_version_local.sh clique >> /dev/null)
	$(DOCKER) build --tag zmk --file Dockerfile .
	$(DOCKER) run --rm $(DOCKER_TTY) --name zmk \
		-v $(PWD)/firmware:/app/firmware$(SELINUX1) \
		-v $(PWD)/config:/app/config:ro$(SELINUX2) \
		-v $(PWD)/zmk-modules:/app/extra-modules:ro$(SELINUX2) \
		-e TIMESTAMP=$(TIMESTAMP) \
		-e COMMIT=$(COMMIT) \
		-e HOST_UID=$(shell id -u) -e HOST_GID=$(shell id -g) \
		-e TEST=$(TEST) \
		-e EXTRA_CMAKE='$(EXTRA_CMAKE)' \
		-e ZEPHYR_PATCH='$(ZEPHYR_PATCH)' \
		-e RADIO=$(RADIO) -e SPLIT_RADIO_KEY=$(SPLIT_RADIO_KEY) \
		-e BUILD_RIGHT=true \
		zmk
	git checkout config/version.dtsi

left:
	$(shell bin/get_version_local.sh clique >> /dev/null)
	$(DOCKER) build --tag zmk --file Dockerfile .
	$(DOCKER) run --rm $(DOCKER_TTY) --name zmk \
		-v $(PWD)/firmware:/app/firmware$(SELINUX1) \
		-v $(PWD)/config:/app/config:ro$(SELINUX2) \
		-v $(PWD)/zmk-modules:/app/extra-modules:ro$(SELINUX2) \
		-e TIMESTAMP=$(TIMESTAMP) \
		-e COMMIT=$(COMMIT) \
		-e HOST_UID=$(shell id -u) -e HOST_GID=$(shell id -g) \
		-e TEST=$(TEST) \
		-e EXTRA_CMAKE='$(EXTRA_CMAKE)' \
		-e ZEPHYR_PATCH='$(ZEPHYR_PATCH)' \
		-e RADIO=$(RADIO) -e SPLIT_RADIO_KEY=$(SPLIT_RADIO_KEY) \
		-e BUILD_RIGHT=false \
		zmk
	git checkout config/version.dtsi

# Test flavor with the key-monitor port (streams + injects key presses)
test:
	$(MAKE) all TEST=1

clean_firmware:
	rm -f firmware/*.uf2

clean_image:
	$(DOCKER) image rm zmk docker.io/zmkfirmware/zmk-build-arm:stable

clean: clean_firmware clean_image
