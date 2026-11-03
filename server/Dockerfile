FROM python:3.12-slim
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1 NXPCTL_DATABASE=/var/lib/nxpctl/nxpctl.sqlite3
WORKDIR /app
COPY server/requirements.txt /app/requirements.txt
RUN pip install --no-cache-dir -r /app/requirements.txt && useradd --system --uid 10001 --create-home nxpctl
COPY server /app/server
RUN mkdir -p /var/lib/nxpctl && chown -R nxpctl:nxpctl /var/lib/nxpctl /app
USER nxpctl
EXPOSE 8080
CMD ["uvicorn", "server.app:app", "--host", "0.0.0.0", "--port", "8080", "--proxy-headers", "--forwarded-allow-ips", "127.0.0.1"]
