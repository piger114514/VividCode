#include "Greeter.h"
#include <iostream>

int main()
{
    Greeter greeter("vividcode");
    std::cout << greeter.message() << '\n';
    return 0;
}
