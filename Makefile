.PHONY: help up down logs logs-engine logs-gateway clean

help:
	@echo 'Usage: make [target]'
	@awk 'BEGIN {FS = ":.*?## "} /^[a-zA-Z_-]+:.*?## / {printf "  %-15s %s\n", $$1, $$2}' $(MAKEFILE_LIST)

up:
	sudo docker-compose up --build

down:
	sudo docker-compose down

logs:
	sudo docker-compose logs -f

logs-engine:
	sudo docker-compose logs -f engine

logs-gateway:
	sudo docker-compose logs -f gateway

clean:
	cd engine && $(MAKE) clean
	rm -rf engine/blocks/* engine/manifests/* engine/owners/*
	rm -rf gateway/data/* gateway/__pycache__ gateway/**/__pycache__ gateway/**/*.pyc
	rm -f /tmp/engine.sock
