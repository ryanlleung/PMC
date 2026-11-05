# The control test GUI in a container: docker build -t pmc-gui tools/pmc-gui
# docker run --rm -p 8080:8080 pmc-gui 192.168.1.100   then open http://localhost:8080
FROM python:3.12-slim
WORKDIR /app
COPY pmc_gui.py pmc_gui.html ./
EXPOSE 8080
ENTRYPOINT ["python", "pmc_gui.py", "--bind", "0.0.0.0"]
CMD ["192.168.1.100"]
