FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends g++ libsqlite3-dev ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY server.cpp .
COPY static ./static
RUN g++ -std=c++17 -pthread server.cpp -lsqlite3 -o medevac_server
EXPOSE 18080
CMD ["./medevac_server"]
