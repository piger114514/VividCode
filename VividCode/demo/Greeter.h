#pragma once
#include <string>

// 演示：头文件声明一个简单的类。
class Greeter
{
public:
    explicit Greeter(std::string name);
    std::string message() const;
private:
    std::string name_;
};
