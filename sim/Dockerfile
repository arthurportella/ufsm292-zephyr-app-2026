FROM python:3.12-slim

ENV PYTHONUNBUFFERED=1
WORKDIR /app

COPY sim.py .

# 5500 = gateway (grupo B) | 5501 = servidor (grupo C)
EXPOSE 5500 5501

CMD ["python", "sim.py"]
