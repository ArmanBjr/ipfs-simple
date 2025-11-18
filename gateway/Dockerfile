FROM python:3.12-slim
WORKDIR /gateway
COPY requirements.txt .
RUN pip install -r requirements.txt
COPY . .
RUN pytest tests/ -v || exit 1
EXPOSE 8000
CMD ["uvicorn", "main:app", "--host", "0.0.0.0", "--port", "8000", "--reload"]
