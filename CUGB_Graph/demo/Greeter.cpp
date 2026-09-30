#include "Greeter.h"
#include <utility>

Greeter::Greeter(std::string name) : name_(std::move(name)) {}

std::string Greeter::message() const
{
    return "Hello, " + name_ + "!";
}
