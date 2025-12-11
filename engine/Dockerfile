FROM gcc:12.2

WORKDIR /app

COPY deps/ ./deps/
COPY include/ ./include/
COPY src/ ./src/
COPY Makefile ./

RUN make

CMD ["./engine", "/tmp/engine.sock"]
