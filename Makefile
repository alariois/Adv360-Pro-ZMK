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

.PHONY: all left clean_firmware clean_image clean

all:
	$(shell bin/get_version_local.sh clique >> /dev/null)
	$(DOCKER) build --tag zmk --file Dockerfile .
	$(DOCKER) run --rm $(DOCKER_TTY) --name zmk \
		-v $(PWD)/firmware:/app/firmware$(SELINUX1) \
		-v $(PWD)/config:/app/config:ro$(SELINUX2) \
		-v $(PWD)/modules:/app/extra-modules:ro$(SELINUX2) \
		-e TIMESTAMP=$(TIMESTAMP) \
		-e COMMIT=$(COMMIT) \
		-e HOST_UID=$(shell id -u) -e HOST_GID=$(shell id -g) \
		-e BUILD_RIGHT=true \
		zmk
	git checkout config/version.dtsi

left:
	$(shell bin/get_version_local.sh clique >> /dev/null)
	$(DOCKER) build --tag zmk --file Dockerfile .
	$(DOCKER) run --rm $(DOCKER_TTY) --name zmk \
		-v $(PWD)/firmware:/app/firmware$(SELINUX1) \
		-v $(PWD)/config:/app/config:ro$(SELINUX2) \
		-v $(PWD)/modules:/app/extra-modules:ro$(SELINUX2) \
		-e TIMESTAMP=$(TIMESTAMP) \
		-e COMMIT=$(COMMIT) \
		-e HOST_UID=$(shell id -u) -e HOST_GID=$(shell id -g) \
		-e BUILD_RIGHT=false \
		zmk
	git checkout config/version.dtsi

clean_firmware:
	rm -f firmware/*.uf2

clean_image:
	$(DOCKER) image rm zmk docker.io/zmkfirmware/zmk-build-arm:stable

clean: clean_firmware clean_image
