using System;

namespace Demo
{
    /// <summary>演示类：用于验证 vividcode 读取 C# 源码。</summary>
    public class Greeter
    {
        private readonly string _name;

        public Greeter(string name)
        {
            _name = name;
        }

        public void Greet()
        {
            Console.WriteLine($"Hello, {_name}!");
        }
    }
}
