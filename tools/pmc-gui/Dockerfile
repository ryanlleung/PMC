# The control test GUI in a container. Build from tools/ (it imports gain-schedule/pmc_gains.py):
#   docker build -t pmc-gui -f tools/pmc-gui/Dockerfile tools
#   docker run --rm -p 8080:8080 pmc-gui 192.168.1.100      then open http://localhost:8080
FROM python:3.12-slim
WORKDIR /app
COPY pmc-gui/pmc_gui.py pmc-gui/pmc_gui.html gain-schedule/pmc_gains.py ./
EXPOSE 8080
ENTRYPOINT ["python", "pmc_gui.py", "--bind", "0.0.0.0"]
CMD ["192.168.1.100"]
