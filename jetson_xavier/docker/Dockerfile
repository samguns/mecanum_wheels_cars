FROM python:3.12-slim

# Use huaweicloud's mirror for faster package installation
RUN sed -i "s@http://.*deb.debian.org@https://repo.huaweicloud.com@g" /etc/apt/sources.list.d/debian.sources

# Install necessary packages
RUN apt-get update && apt-get install -y \
    python3-pip

RUN pip config --user set global.index https://mirrors.huaweicloud.com/repository/pypi
RUN pip config --user set global.index-url https://mirrors.huaweicloud.com/repository/pypi/simple
RUN pip config --user set global.trusted-host mirrors.huaweicloud.com

WORKDIR /app
COPY backend .
RUN pip install -r /app/requirements.txt

CMD ["python", "/app/socketio_server.py"]