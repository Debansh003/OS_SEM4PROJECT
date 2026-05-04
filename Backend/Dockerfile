FROM gcc:latest

WORKDIR /app
COPY server.c .

RUN gcc server.c -o server

EXPOSE 8080

CMD ["./server"]
