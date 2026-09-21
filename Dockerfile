# Lords Mobile bot + web console. Storage: SQL Server (see docs/deployment.md).
FROM debian:bookworm-slim AS bot
RUN apt-get update && apt-get install -y --no-install-recommends build-essential cmake && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j2

FROM python:3.12-slim
RUN pip install --no-cache-dir "pymssql>=2.3" \
 && useradd --uid 10001 --create-home --shell /usr/sbin/nologin lmbot \
 && mkdir -p /var/lib/lmbot && chown lmbot /var/lib/lmbot
WORKDIR /app
COPY webui ./webui
COPY tools ./tools
COPY --from=bot /src/build/client /app/client
ENV LMBOT_CLIENT=/app/client \
    LMBOT_UI_ROOT=/var/lib/lmbot \
    LMBOT_HOST=0.0.0.0 \
    PYTHONUNBUFFERED=1
USER 10001
EXPOSE 8765
VOLUME /var/lib/lmbot
CMD ["python", "webui/server.py", "--no-browser", "--port", "8765"]
