.PHONY: help up down clean

help:
	@echo 'Usage: make [target]'
	@awk 'BEGIN {FS = ":.*?## "} /^[a-zA-Z_-]+:.*?## / {printf "  %-15s %s\n", $$1, $$2}' $(MAKEFILE_LIST)

up:
	sudo docker-compose up --build

down:
	sudo docker-compose down

clean:
	cd engine && $(MAKE) clean
	rm -rf engine/blocks/* engine/manifests/* engine/owners/*
	rm -rf gateway/data/* gateway/__pycache__ gateway/**/__pycache__ gateway/**/*.pyc
	rm -f /tmp/engine.sock
