class Greeter:
    """演示类：用于验证 vividcode 读取 Python 源码。"""

    def __init__(self, name: str) -> None:
        self.name = name

    def greet(self) -> None:
        print(f"Hello, {self.name}!")
