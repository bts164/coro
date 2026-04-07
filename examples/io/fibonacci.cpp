#include <coro/coro.h>
#include <coro/coro_stream.h>
#include <coro/runtime/runtime.h>
#include <iostream>

coro::CoroStream<int> fibonacci(int x0, int x1) {
    co_yield x0;
    co_yield x1;
    while (true) {
        int xn = x0 + x1;
        co_yield xn;
        x0 = x1;
        x1 = xn;
    }
    co_return;
}

coro::Coro<void> run() {
    coro::CoroStream<int> fib0 = fibonacci(0, 1);
    coro::CoroStream<int> fib1 = fibonacci(1, 2);
    coro::CoroStream<int> fib2 = fibonacci(2, 3);
    coro::CoroStream<int> fib3 = fibonacci(3, 4);
    for (size_t i = 0; i < 10; ++i) {
        std::cout
            << (co_await coro::next(fib0)).value() << ","
            << (co_await coro::next(fib1)).value() << ","
            << (co_await coro::next(fib2)).value() << ","
            << (co_await coro::next(fib3)).value() << "\n";
    }
    co_return;
}

int main() {
    coro::Runtime rt;
    rt.block_on(run());
}
