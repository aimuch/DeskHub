class AgentError(Exception):
    def __init__(self, code: str, message: str, status: int = 400, retryable: bool = False):
        super().__init__(message)
        self.code = code
        self.message = message
        self.status = status
        self.retryable = retryable

    def body(self, request_id: str | None = None) -> dict:
        return {"error": {"code": self.code, "message": self.message,
                          "retryable": self.retryable, "request_id": request_id}}
